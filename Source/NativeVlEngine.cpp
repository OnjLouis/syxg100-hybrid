#include "NativeVlEngine.h"

#include "LeImageLoader.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace hybrid {
namespace {

constexpr std::uint32_t nativeVlPolyphony = 1;

constexpr std::size_t imageSize = 0x001aecdc;
constexpr std::size_t imageReservationStep = 0x00200000;
constexpr std::uintptr_t firstImageBase = 0x10000000;
constexpr std::uintptr_t lastImageBase = 0x60000000;
constexpr std::size_t workspaceSize = 0x00100000;
constexpr std::uintptr_t firstWorkspaceBase = 0x00500000;
constexpr std::uintptr_t lastWorkspaceBase = 0x08000000;
constexpr std::size_t transactionThunkOffset = 0x00020000;
constexpr std::size_t transactionThunkSize = 52;
constexpr std::size_t descriptorOffset = 0x00030000;
constexpr std::size_t pointerTableOffset = 0x0003f000;
constexpr std::size_t renderBufferOffset = 0x00040000;
constexpr std::size_t planeStride = 0x2000;
constexpr std::uint32_t defaultNativeRenderQuantum = 256;
constexpr std::uintptr_t criticalInitOffset = 0x00172100;
constexpr std::uintptr_t deviceInitOffset = 0x00172110;
constexpr std::uintptr_t commandDispatcherOffset = 0x001721b0;
constexpr std::uintptr_t renderStateInitOffset = 0x0016d9c1;
constexpr std::uintptr_t callbackPrepareOffset = 0x00197620;
constexpr std::uintptr_t callbackInstallOffset = 0x00195730;
constexpr std::uintptr_t callbackSourceOffset = 0x00095ee0;
constexpr std::uintptr_t callbackSourcePointerOffset = 0x000e4200;
constexpr std::uintptr_t callbackPointerOffset = 0x000f1f60;
constexpr std::uintptr_t backendGateOffset = 0x00159ee0;
constexpr std::uintptr_t backendAvailableOffset = 0x00159fcc;
constexpr std::uintptr_t generatedCodeOffset = 0x0015a02c;
constexpr std::uintptr_t generatedHeapOffset = 0x00159fbc;
constexpr std::uintptr_t generatedCallbackBaseOffset = 0x0016db70;
constexpr std::uintptr_t backendCoefficientUploadOffset = 0x001a6dcf;
constexpr std::uintptr_t backendCoefficientUploadCompleteOffset = 0x001a6dfc;
constexpr std::uint32_t backendCoefficientUploadSize = 0x00000a60;
constexpr std::uintptr_t backendTransportReadbackOffset = 0x001a6e20;
constexpr std::uintptr_t backendTransportEpilogueOffset = 0x001a6e94;
constexpr std::uintptr_t backendTransactionPointerTableStackOffset = 0x14;
constexpr std::uintptr_t backendTransactionFrameCountStackOffset = 0x1c;
constexpr std::array<std::uintptr_t, 4> captureOffsets {
    0x001a6e13, 0x001a6e17, 0x001a6e1b, 0x001a6e1f,
};
constexpr std::array<std::uint8_t, 7> generatedAllocatorJitter {
    0x0f, 0x31, 0x25, 0xff, 0x01, 0x00, 0x00
};
constexpr std::array<std::uint8_t, 7> zeroGeneratedAllocatorJitter {
    0x31, 0xc0, 0x90, 0x90, 0x90, 0x90, 0x90
};
constexpr std::uintptr_t generatedAllocatorJitterOffset = 0x4da;
constexpr std::uintptr_t generatedDiagnosticClearOffset = 0x4c3;
constexpr std::array<std::uint8_t, 3> generatedDiagnosticClearStore {
    0x0f, 0x7f, 0x07
};

enum class GeneratedHeapMode {
    restore,
    advance,
};

enum class GeneratedJitterMode {
    zero,
    native,
};

enum class BackendCompletionMode {
    skip,
    zero,
};

enum class RenderPath {
    transport,
    direct,
};

GeneratedHeapMode readGeneratedHeapMode()
{
    wchar_t value[16] {};
    const auto length = GetEnvironmentVariableW(
        L"SYXG100_VL_GENERATED_HEAP", value, static_cast<DWORD>(std::size(value)));
    if (length == 0)
        return GeneratedHeapMode::restore;
    if (length >= std::size(value))
        throw std::runtime_error("SYXG100_VL_GENERATED_HEAP is too long");
    if (_wcsicmp(value, L"restore") == 0)
        return GeneratedHeapMode::restore;
    if (_wcsicmp(value, L"advance") == 0)
        return GeneratedHeapMode::advance;
    throw std::runtime_error(
        "SYXG100_VL_GENERATED_HEAP must be restore or advance");
}

GeneratedJitterMode readGeneratedJitterMode()
{
    wchar_t value[16] {};
    const auto length = GetEnvironmentVariableW(
        L"SYXG100_VL_GENERATED_JITTER", value,
        static_cast<DWORD>(std::size(value)));
    if (length == 0)
        return GeneratedJitterMode::zero;
    if (length >= std::size(value))
        throw std::runtime_error("SYXG100_VL_GENERATED_JITTER is too long");
    if (_wcsicmp(value, L"zero") == 0)
        return GeneratedJitterMode::zero;
    if (_wcsicmp(value, L"native") == 0)
        return GeneratedJitterMode::native;
    throw std::runtime_error(
        "SYXG100_VL_GENERATED_JITTER must be zero or native");
}

BackendCompletionMode readBackendCompletionMode()
{
    wchar_t value[16] {};
    const auto length = GetEnvironmentVariableW(
        L"SYXG100_VL_BACKEND_COMPLETION", value,
        static_cast<DWORD>(std::size(value)));
    if (length == 0)
        return BackendCompletionMode::skip;
    if (length >= std::size(value))
        throw std::runtime_error("SYXG100_VL_BACKEND_COMPLETION is too long");
    if (_wcsicmp(value, L"skip") == 0)
        return BackendCompletionMode::skip;
    if (_wcsicmp(value, L"zero") == 0)
        return BackendCompletionMode::zero;
    throw std::runtime_error(
        "SYXG100_VL_BACKEND_COMPLETION must be skip or zero");
}

RenderPath readRenderPath()
{
    wchar_t value[16] {};
    const auto length = GetEnvironmentVariableW(
        L"SYXG100_VL_RENDER_PATH", value, static_cast<DWORD>(std::size(value)));
    if (length == 0)
        return RenderPath::direct;
    if (length >= std::size(value))
        throw std::runtime_error("SYXG100_VL_RENDER_PATH is too long");
    if (_wcsicmp(value, L"transport") == 0)
        return RenderPath::transport;
    if (_wcsicmp(value, L"direct") == 0)
        return RenderPath::direct;
    throw std::runtime_error(
        "SYXG100_VL_RENDER_PATH must be transport or direct");
}

std::uint32_t readNativeRenderQuantum()
{
    wchar_t value[16] {};
    const auto length = GetEnvironmentVariableW(
        L"SYXG100_VL_QUANTUM", value, static_cast<DWORD>(std::size(value)));
    if (length == 0)
        return defaultNativeRenderQuantum;
    if (length >= std::size(value))
        throw std::runtime_error("SYXG100_VL_QUANTUM is too long");
    const auto quantum = static_cast<std::uint32_t>(_wtoi(value));
    if (quantum == 64 || quantum == 128 || quantum == 256 || quantum == 512)
        return quantum;
    throw std::runtime_error("SYXG100_VL_QUANTUM must be 64, 128, 256, or 512");
}

std::uint32_t call0(std::uintptr_t address)
{
    return reinterpret_cast<std::uint32_t(__cdecl*)()>(address)();
}

std::uint32_t call1(std::uintptr_t address, std::uint32_t first)
{
    return reinterpret_cast<std::uint32_t(__cdecl*)(std::uint32_t)>(address)(first);
}

std::uint32_t call2(std::uintptr_t address, std::uint32_t first,
                    std::uint32_t second)
{
    return reinterpret_cast<std::uint32_t(__cdecl*)(
        std::uint32_t, std::uint32_t)>(address)(first, second);
}

std::uint32_t call3(std::uintptr_t address, std::uint32_t first,
                    std::uint32_t second, std::uint32_t third)
{
    return reinterpret_cast<std::uint32_t(__cdecl*)(
        std::uint32_t, std::uint32_t, std::uint32_t)>(address)(first, second,
                                                               third);
}

void write32(std::uintptr_t address, std::uint32_t value)
{
    *reinterpret_cast<volatile std::uint32_t*>(address) = value;
}

std::uint32_t read32(std::uintptr_t address)
{
    return *reinterpret_cast<volatile const std::uint32_t*>(address);
}

void logUnhandledException(const EXCEPTION_RECORD& record,
                           const CONTEXT& context,
                           std::uintptr_t imageBase,
                           std::uint32_t descriptorFrames,
                           const std::array<std::uint32_t, 4>& transportArguments,
                           std::uint32_t rendererCommand)
{
    wchar_t logPath[MAX_PATH] {};
    const auto pathLength = GetEnvironmentVariableW(
        L"SYXG100_HYBRID_LOG", logPath, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH)
        return;
    char instructionBytes[64] {};
    auto* byteCursor = instructionBytes;
    auto remaining = sizeof(instructionBytes);
    const auto* instruction = reinterpret_cast<const std::uint8_t*>(context.Eip);
    for (std::size_t index = 0; index < 12 && remaining > 3; ++index) {
        const auto used = std::snprintf(byteCursor, remaining, "%02x ",
                                        instruction[index]);
        if (used <= 0 || static_cast<std::size_t>(used) >= remaining)
            break;
        byteCursor += used;
        remaining -= static_cast<std::size_t>(used);
    }
    char message[960] {};
    const auto imageOffset = context.Eip >= imageBase
        && context.Eip < imageBase + imageSize
        ? context.Eip - imageBase : static_cast<std::uintptr_t>(-1);
    const auto operation = record.NumberParameters >= 1
        ? record.ExceptionInformation[0] : 0;
    const auto target = record.NumberParameters >= 2
        ? record.ExceptionInformation[1] : 0;
    const auto length = std::snprintf(
        message, sizeof(message),
        "PVL unhandled exception code=%08lx eip=%08lx image-offset=%08lx "
        "esp=%08lx operation=%08lx target=%08lx eax=%08lx ebx=%08lx "
        "ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx "
        "descriptor-frames=%lu tx-args=%08lx,%08lx,%08lx,%08lx "
        "renderer=%08lx bytes=%s\n",
        record.ExceptionCode, context.Eip,
        static_cast<unsigned long>(imageOffset), context.Esp,
        static_cast<unsigned long>(operation),
        static_cast<unsigned long>(target), context.Eax, context.Ebx,
        context.Ecx, context.Edx, context.Esi, context.Edi, context.Ebp,
        static_cast<unsigned long>(descriptorFrames),
        static_cast<unsigned long>(transportArguments[0]),
        static_cast<unsigned long>(transportArguments[1]),
        static_cast<unsigned long>(transportArguments[2]),
        static_cast<unsigned long>(transportArguments[3]),
        static_cast<unsigned long>(rendererCommand),
        instructionBytes);
    if (length <= 0)
        return;
    const auto file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, message,
              static_cast<DWORD>(std::min<std::size_t>(length,
                                                       sizeof(message) - 1)),
              &written, nullptr);
    CloseHandle(file);
}

void dumpGeneratedCode(std::uint32_t transactionIndex,
                       std::uint32_t renderer,
                       std::uint32_t sampleCallback,
                       std::uint32_t backendTransport)
{
    if (transactionIndex != 0)
        return;

    wchar_t pathPrefix[MAX_PATH] {};
    const auto pathLength = GetEnvironmentVariableW(
        L"SYXG100_VL_CODE_DUMP", pathPrefix, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH - 32)
        return;

    const auto dump = [&](const wchar_t* suffix, std::uint32_t address) {
        wchar_t path[MAX_PATH] {};
        std::swprintf(path, std::size(path), L"%ls-%ls.bin", pathPrefix, suffix);
        const auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
                                      nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        constexpr DWORD dumpSize = 0x1000;
        DWORD written = 0;
        WriteFile(file, reinterpret_cast<const void*>(address), dumpSize,
                  &written, nullptr);
        CloseHandle(file);
    };

    dump(L"renderer", renderer);
    dump(L"sample-callback", sampleCallback);
    dump(L"backend-transport", backendTransport);
}

void logTransactionCapture(std::uint32_t descriptorFrames,
                           std::uintptr_t transportFrame,
                           std::uint32_t transactionIndex,
                           std::uint32_t renderer,
                           std::uint32_t sampleCallback,
                           std::uint32_t serviceEntryRingPhase,
                           std::uint32_t ringPhase,
                           std::uint32_t sampleRenderer,
                           std::uint32_t generatedCode,
                           std::uint32_t generatedHeap,
                           std::uint32_t controlRingBegin,
                           std::uint32_t controlRingEnd,
                           std::uint32_t controlRingWrite,
                           std::uint32_t controlRingRead,
                           std::uint32_t controlRingAux)
{
    wchar_t logPath[MAX_PATH] {};
    const auto pathLength = GetEnvironmentVariableW(
        L"SYXG100_VL_TRANSACTION_LOG", logPath, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH || transactionIndex >= 64)
        return;

    std::array<std::uint32_t, 7> stackValues {};
    for (std::size_t index = 0; index < stackValues.size(); ++index) {
        stackValues[index] = *reinterpret_cast<const std::uint32_t*>(
            transportFrame + 0x08 + index * sizeof(std::uint32_t));
    }
    char message[512] {};
    const auto length = std::snprintf(
        message, sizeof(message),
        "tx=%lu descriptor=%lu ebx=%08lx "
        "+08=%08lx +0c=%08lx +10=%08lx +14=%08lx +18=%08lx "
        "+1c=%08lx +20=%08lx renderer=%08lx callback=%08lx "
        "ring-entry=%08lx ring-generated=%08lx sample-renderer=%08lx "
        "code=%08lx heap=%08lx "
        "control=%08lx,%08lx,%08lx,%08lx,%08lx\n",
        static_cast<unsigned long>(transactionIndex),
        static_cast<unsigned long>(descriptorFrames),
        static_cast<unsigned long>(transportFrame),
        static_cast<unsigned long>(stackValues[0]),
        static_cast<unsigned long>(stackValues[1]),
        static_cast<unsigned long>(stackValues[2]),
        static_cast<unsigned long>(stackValues[3]),
        static_cast<unsigned long>(stackValues[4]),
        static_cast<unsigned long>(stackValues[5]),
        static_cast<unsigned long>(stackValues[6]),
        static_cast<unsigned long>(renderer),
        static_cast<unsigned long>(sampleCallback),
        static_cast<unsigned long>(serviceEntryRingPhase),
        static_cast<unsigned long>(ringPhase),
        static_cast<unsigned long>(sampleRenderer),
        static_cast<unsigned long>(generatedCode),
        static_cast<unsigned long>(generatedHeap),
        static_cast<unsigned long>(controlRingBegin),
        static_cast<unsigned long>(controlRingEnd),
        static_cast<unsigned long>(controlRingWrite),
        static_cast<unsigned long>(controlRingRead),
        static_cast<unsigned long>(controlRingAux));
    if (length <= 0)
        return;

    const auto file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, message,
              static_cast<DWORD>(std::min<std::size_t>(length,
                                                       sizeof(message) - 1)),
              &written, nullptr);
    CloseHandle(file);
}

} // namespace

class NativeVlEngine::Impl {
public:
    Impl(const std::filesystem::path& vxdPath, std::uint32_t sampleRate)
    {
        try {
            generatedHeapMode = readGeneratedHeapMode();
            generatedJitterMode = readGeneratedJitterMode();
            backendCompletionMode = readBackendCompletionMode();
            renderPath = readRenderPath();
            nativeRenderQuantum = readNativeRenderQuantum();
            installExceptionRouter();
            allocateImage(vxdPath);
            allocateWorkspace();

            ActiveScope scope(this);
            if (call0(address(criticalInitOffset)) != 1
                || call0(address(deviceInitOffset)) != 1) {
                throw std::runtime_error("PVL VxD initialization failed");
            }
            deviceInitialized = true;
            callbackThread = CreateThread(nullptr, 0, callbackThreadMain, this,
                                          0, nullptr);
            if (callbackThread == nullptr)
                throw std::runtime_error("cannot create VL callback thread");
            const auto callbackDeadline = GetTickCount64() + 10'000;
            while (!callbackReady.load(std::memory_order_acquire)
                   && GetTickCount64() < callbackDeadline) {
                Sleep(1);
            }
            if (!callbackReady.load(std::memory_order_acquire))
                throw std::runtime_error("VL callback initialization timed out");
            if (SuspendThread(callbackThread) == static_cast<DWORD>(-1))
                throw std::runtime_error("cannot suspend VL callback keeper");
            callbackSuspended = true;
            enableCallbackStackCode();
            enableTransactionThunkCode();
            call0(address(callbackInstallOffset));
            call2(dispatcher(), 13, nativeVlPolyphony);
            call0(address(renderStateInitOffset));
            call3(dispatcher(), 4, sampleRate, sampleRate);
        } catch (...) {
            release();
            throw;
        }
    }

    ~Impl() { release(); }

private:
    class ActiveScope {
    public:
        explicit ActiveScope(Impl* engine) : previous(activeEngine)
        {
            activeEngine = engine;
        }
        ~ActiveScope() { activeEngine = previous; }

    private:
        Impl* previous;
    };

    void release() noexcept
    {
        if (deviceInitialized) {
            ActiveScope scope(this);
            call1(dispatcher(), 3);
            deviceInitialized = false;
        }
        stopCallback.store(true, std::memory_order_release);
        if (callbackThread != nullptr && callbackSuspended) {
            ResumeThread(callbackThread);
            callbackSuspended = false;
        }
        if (callbackThread != nullptr) {
            WaitForSingleObject(callbackThread, 5'000);
            CloseHandle(callbackThread);
        }
        if (workspaceBase != 0)
            VirtualFree(reinterpret_cast<void*>(workspaceBase), 0, MEM_RELEASE);
        if (imageBase != 0)
            VirtualFree(reinterpret_cast<void*>(imageBase), 0, MEM_RELEASE);
        callbackThread = nullptr;
        workspaceBase = 0;
        imageBase = 0;
    }

public:

    void sendShort(std::uint32_t packedMessage)
    {
        ActiveScope scope(this);
        call2(dispatcher(), 5, packedMessage);
    }

    void sendSysex(std::span<const std::uint8_t> bytes)
    {
        const std::array<std::uint32_t, 2> header {
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(bytes.data())),
            static_cast<std::uint32_t>(bytes.size()),
        };
        ActiveScope scope(this);
        call2(dispatcher(), 6,
              static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(
                  header.data())));
    }

    void setSampleRate(std::uint32_t sampleRate)
    {
        ActiveScope scope(this);
        call3(dispatcher(), 4, sampleRate, sampleRate);
    }

    void prepare()
    {
        warmUp();
    }

    void warmUp()
    {
        if (rendererWarm)
            return;
        ActiveScope scope(this);
        warmUpRenderer();
    }

    void render(std::uint32_t frames)
    {
        if (frames == 0 || frames > NativeVlEngine::maxFrames)
            throw std::runtime_error("invalid VL render block size");
        ActiveScope scope(this);
        std::memset(reinterpret_cast<void*>(renderBuffer()), 0,
                    planeStride * NativeVlEngine::planeCount);
        if (renderPath == RenderPath::direct) {
            std::uint32_t renderedFrames = 0;
            while (renderedFrames < frames) {
                const auto quantumFrames = std::min(
                    frames - renderedFrames, nativeRenderQuantum);
                initializeRenderDescriptor(quantumFrames, renderedFrames);
                write32(address(backendGateOffset), 0);
                call2(dispatcher(), 7,
                      static_cast<std::uint32_t>(workspaceBase
                                                 + descriptorOffset));
                renderedFrames += quantumFrames;
            }
            return;
        }
        std::uint32_t renderedFrames = 0;
        while (renderedFrames < frames) {
            const auto quantumFrames = std::min(
                frames - renderedFrames, nativeRenderQuantum);
            std::uint32_t quantumRenderedFrames = 0;
            while (quantumRenderedFrames < quantumFrames) {
                const auto transactionRequest =
                    quantumFrames - quantumRenderedFrames;
                initializeRenderDescriptor(transactionRequest, renderedFrames);
                captureAndRender();
                if (lastTransactionFrames == 0
                    || lastTransactionFrames > transactionRequest) {
                    throw std::runtime_error(
                        "invalid VL transaction frame count");
                }
                quantumRenderedFrames += lastTransactionFrames;
                renderedFrames += lastTransactionFrames;
            }
        }
    }

    std::span<const std::int16_t> plane(std::size_t index,
                                        std::uint32_t frames) const
    {
        if (index >= NativeVlEngine::planeCount
            || frames > NativeVlEngine::maxFrames) {
            return {};
        }
        return { reinterpret_cast<const std::int16_t*>(
                     renderBuffer() + index * planeStride),
                 static_cast<std::size_t>(frames) * 2 };
    }

    LONG handleException(EXCEPTION_POINTERS* details)
    {
        const auto* record = details->ExceptionRecord;
        auto* context = details->ContextRecord;
        const auto* instruction = reinterpret_cast<const std::uint8_t*>(
            context->Eip);
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
            && record->NumberParameters >= 2
            && record->ExceptionInformation[0] == 8
            && context->Eip >= context->Esp - 0x10000
            && context->Eip <= context->Esp + 0x10000) {
            SYSTEM_INFO systemInfo {};
            GetSystemInfo(&systemInfo);
            const auto pageSize = static_cast<std::uintptr_t>(systemInfo.dwPageSize);
            auto* page = reinterpret_cast<void*>(context->Eip & ~(pageSize - 1));
            DWORD oldProtection = 0;
            if (VirtualProtect(page, pageSize, PAGE_EXECUTE_READWRITE,
                               &oldProtection)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        if (record->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION) {
            logUnhandledException(*record, *context, imageBase,
                                  lastDescriptorFrames,
                                  lastTransportArguments, rendererCommand);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const auto instructionOffset = context->Eip - imageBase;
        if (captureActive.load()
            && instructionOffset == backendCoefficientUploadOffset) {
            // Port writes have no software consumer. Skip the upload loop while
            // retaining Yamaha's command generation and render transaction.
            context->Esi = backendCoefficientUploadSize;
            context->Eip = address(backendCoefficientUploadCompleteOffset);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (instruction[0] == 0xec || instruction[0] == 0xed
            || instruction[0] == 0xe4 || instruction[0] == 0xe5) {
            std::uint32_t value = captureActive.load() ? 5u : 0u;
            if (captureActive.load()
                && backendCompletionMode == BackendCompletionMode::zero
                && instructionOffset >= backendTransportReadbackOffset
                && instructionOffset < backendTransportEpilogueOffset) {
                const auto engineBase = read32(address(0x0016d7b8));
                const auto dataPort = static_cast<std::uint16_t>(
                    read32(engineBase + sizeof(std::uint32_t)));
                if (static_cast<std::uint16_t>(context->Edx) == dataPort)
                    value = 0;
            }
            context->Eax = (context->Eax & 0xffffff00u) | value;
            context->Eip += instruction[0] == 0xe4 || instruction[0] == 0xe5
                ? 2 : 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0xee || instruction[0] == 0xef
            || instruction[0] == 0xe6 || instruction[0] == 0xe7) {
            const auto offset = context->Eip - imageBase;
            for (std::size_t index = 0; index < captureOffsets.size(); ++index) {
                if (offset != captureOffsets[index])
                    continue;
                capturedRendererBytes[index] = static_cast<std::uint8_t>(context->Eax);
                ++capturedRendererByteCount;
                if (index + 1 == captureOffsets.size() && captureActive.load()) {
                    std::memcpy(&rendererCommand, capturedRendererBytes.data(),
                                sizeof(rendererCommand));
                    write32(address(backendGateOffset), 0);
                    const auto* descriptor = reinterpret_cast<const std::uint32_t*>(
                        workspaceBase + descriptorOffset);
                    lastDescriptorFrames = descriptor[1];
                    const auto generatedCode = read32(address(generatedCodeOffset));
                    const auto sampleCallback = static_cast<std::uint32_t>(
                        (address(generatedCallbackBaseOffset) + generatedCode + 7)
                        & ~std::uintptr_t { 7 });
                    diagnosticSampleCallback = sampleCallback;
                    const auto engineBase = read32(address(0x0016d7b8));
                    const auto controlRingBegin = engineBase + 0x1d34;
                    logTransactionCapture(lastDescriptorFrames, context->Ebx,
                                          transactionLogCount++, rendererCommand,
                                          sampleCallback,
                                          serviceEntryRingPhase,
                                          read32(address(0x0016d7bc)),
                                          read32(engineBase + 0x16f8),
                                          generatedCode,
                                          read32(address(generatedHeapOffset)),
                                          controlRingBegin,
                                          read32(engineBase + 0x2264),
                                          read32(engineBase + 0x2268),
                                          read32(engineBase + 0x226c),
                                          read32(engineBase + 0x2270));
                    for (std::size_t argument = 0;
                         argument < lastTransportArguments.size(); ++argument) {
                        lastTransportArguments[argument]
                            = *reinterpret_cast<const std::uint32_t*>(
                                context->Ebx + 0x10 + argument * sizeof(std::uint32_t));
                    }
                    // Service 7 advances this table and may split the outer
                    // request, so use the current transaction's positions and
                    // exact frame count.
                    const auto transactionPointerTable =
                        *reinterpret_cast<const std::uint32_t*>(
                            context->Ebx
                            + backendTransactionPointerTableStackOffset);
                    const auto transactionFrames =
                        *reinterpret_cast<const std::uint32_t*>(
                            context->Ebx + backendTransactionFrameCountStackOffset);
                    lastTransactionFrames = transactionFrames;
                    configureGeneratedAllocatorJitter();
                    buildTransactionThunk(transactionPointerTable,
                                          transactionFrames);
                    captureCompleted = true;
                    ++capturedRendererTransactionCount;
                    context->Eip = workspaceBase + transactionThunkOffset;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                break;
            }
            context->Eip += instruction[0] == 0xe6 || instruction[0] == 0xe7
                ? 2 : 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0xfa || instruction[0] == 0xfb) {
            context->Eip += 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0x0f && instruction[1] == 0x20
            && instruction[2] == 0xc0) {
            context->Eax = 0;
            context->Eip += 3;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0x0f && instruction[1] == 0x22
            && instruction[2] == 0xc0) {
            context->Eip += 3;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0x0f && instruction[1] == 0x06) {
            context->Eip += 2;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        logUnhandledException(*record, *context, imageBase,
                              lastDescriptorFrames,
                              lastTransportArguments, rendererCommand);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static LONG WINAPI exceptionRouter(EXCEPTION_POINTERS* details)
    {
        return activeEngine == nullptr
            ? EXCEPTION_CONTINUE_SEARCH : activeEngine->handleException(details);
    }

    static void installExceptionRouter()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            if (AddVectoredExceptionHandler(1, exceptionRouter) == nullptr)
                throw std::runtime_error("cannot install VL exception router");
        });
    }

    void allocateImage(const std::filesystem::path& path)
    {
        for (std::uintptr_t candidate = firstImageBase;
             candidate < lastImageBase; candidate += imageReservationStep) {
            const auto allocation = VirtualAlloc(reinterpret_cast<void*>(candidate),
                                                 imageSize,
                                                 MEM_RESERVE | MEM_COMMIT,
                                                 PAGE_EXECUTE_READWRITE);
            if (allocation == reinterpret_cast<void*>(candidate)) {
                imageBase = candidate;
                break;
            }
        }
        if (imageBase == 0)
            throw std::runtime_error("cannot reserve a PVL image base");
        const auto image = loadLeImage(path, static_cast<std::uint32_t>(imageBase));
        if (image.size() != imageSize)
            throw std::runtime_error("unexpected PVL image size");
        std::copy(image.begin(), image.end(),
                  reinterpret_cast<std::uint8_t*>(imageBase));
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(imageBase),
                              imageSize);
    }

    void allocateWorkspace()
    {
        for (std::uintptr_t candidate = firstWorkspaceBase;
             candidate < lastWorkspaceBase; candidate += workspaceSize) {
            const auto allocation = VirtualAlloc(reinterpret_cast<void*>(candidate),
                                                 workspaceSize,
                                                 MEM_RESERVE | MEM_COMMIT,
                                                 PAGE_READWRITE);
            if (allocation == reinterpret_cast<void*>(candidate)) {
                workspaceBase = candidate;
                break;
            }
        }
        if (workspaceBase == 0)
            throw std::runtime_error("cannot reserve a VL render workspace");
        std::array<std::uint32_t, NativeVlEngine::planeCount> addresses {};
        for (std::size_t index = 0; index < addresses.size(); ++index) {
            addresses[index] = static_cast<std::uint32_t>(
                renderBuffer() + index * planeStride);
        }
        std::memcpy(reinterpret_cast<void*>(pointerTable()), addresses.data(),
                    sizeof(addresses));
    }

    static DWORD WINAPI callbackThreadMain(void* context)
    {
        auto* self = static_cast<Impl*>(context);
        ActiveScope scope(self);
        write32(self->address(callbackSourcePointerOffset),
                static_cast<std::uint32_t>(self->address(callbackSourceOffset)));
        call0(self->address(callbackPrepareOffset));
        self->callbackReady.store(true, std::memory_order_release);
        // Yamaha generated executable callback data in this stack frame. Do not
        // call another function on this thread until rendering has stopped.
        while (!self->stopCallback.load(std::memory_order_acquire))
            YieldProcessor();
        return 0;
    }

    void enableCallbackStackCode()
    {
        const auto callback = read32(address(callbackPointerOffset));
        constexpr std::uintptr_t pageMask = 0xfff;
        const auto firstPage = (callback - 0x10000) & ~pageMask;
        const auto lastPage = (callback + 0x10000) & ~pageMask;
        for (auto page = firstPage; page <= lastPage; page += 0x1000) {
            MEMORY_BASIC_INFORMATION region {};
            if (VirtualQuery(reinterpret_cast<void*>(page), &region,
                             sizeof(region)) == 0
                || region.State != MEM_COMMIT
                || (region.Protect & PAGE_GUARD) != 0) {
                continue;
            }
            DWORD oldProtection = 0;
            VirtualProtect(reinterpret_cast<void*>(page), 0x1000,
                           PAGE_EXECUTE_READWRITE, &oldProtection);
        }
    }

    void enableTransactionThunkCode()
    {
        DWORD oldProtection = 0;
        if (!VirtualProtect(
                reinterpret_cast<void*>(workspaceBase + transactionThunkOffset),
                transactionThunkSize, PAGE_EXECUTE_READWRITE, &oldProtection)) {
            throw std::runtime_error("cannot enable VL transaction thunk memory");
        }
    }

    void normalizeGeneratedAllocators()
    {
        const auto code = read32(address(generatedCodeOffset));
        const auto heap = read32(address(generatedHeapOffset));
        if (heap < code)
            throw std::runtime_error("invalid generated allocator ordering");
        write32(address(generatedCodeOffset), 0);
        write32(address(generatedHeapOffset), heap - code);
    }

    void configureGeneratedAllocatorJitter()
    {
        const auto renderer = static_cast<std::uintptr_t>(rendererCommand);
        const auto imageEnd = imageBase + imageSize;
        if (renderer < imageBase
            || renderer > imageEnd - generatedAllocatorJitterOffset
                                      - generatedAllocatorJitter.size()) {
            throw std::runtime_error("generated renderer lies outside PVL image");
        }
        auto* jitter = reinterpret_cast<std::uint8_t*>(
            renderer + generatedAllocatorJitterOffset);
        if (!std::equal(generatedAllocatorJitter.begin(),
                        generatedAllocatorJitter.end(), jitter)) {
            throw std::runtime_error("generated allocator jitter tail not found");
        }
        if (generatedJitterMode == GeneratedJitterMode::native)
            return;
        std::copy(zeroGeneratedAllocatorJitter.begin(),
                  zeroGeneratedAllocatorJitter.end(), jitter);
        FlushInstructionCache(GetCurrentProcess(), jitter,
                              zeroGeneratedAllocatorJitter.size());

        wchar_t dumpPath[MAX_PATH] {};
        const auto dumpPathLength = GetEnvironmentVariableW(
            L"SYXG100_VL_CODE_DUMP", dumpPath, MAX_PATH);
        if (dumpPathLength == 0 || dumpPathLength >= MAX_PATH)
            return;
        auto* clearStore = reinterpret_cast<std::uint8_t*>(
            renderer + generatedDiagnosticClearOffset);
        if (!std::equal(generatedDiagnosticClearStore.begin(),
                        generatedDiagnosticClearStore.end(), clearStore)) {
            throw std::runtime_error("generated diagnostic clear loop not found");
        }
        std::fill_n(clearStore, generatedDiagnosticClearStore.size(), 0x90);
        FlushInstructionCache(GetCurrentProcess(), clearStore,
                              generatedDiagnosticClearStore.size());
    }

    void buildTransactionThunk(std::uint32_t transactionPointerTable,
                               std::uint32_t frames)
    {
        std::array<std::uint8_t, transactionThunkSize> code {};
        std::size_t cursor = 0;
        auto emit8 = [&](std::uint8_t value) {
            code[cursor++] = value;
        };
        auto emit32 = [&](std::uint32_t value) {
            std::memcpy(code.data() + cursor, &value, sizeof(value));
            cursor += sizeof(value);
        };

        emit8(0x9c);                         // pushfd
        emit8(0x60);                         // pushad
        emit8(0x68); emit32(frames);         // push frames
        emit8(0x6a); emit8(0x00);            // push 0
        emit8(0x68); emit32(transactionPointerTable);
        emit8(0xb8); emit32(rendererCommand);// mov eax, renderer
        emit8(0xff); emit8(0xd0);            // call eax
        emit8(0x83); emit8(0xc4); emit8(0x0c);// add esp, 12
        if (generatedHeapMode == GeneratedHeapMode::restore) {
            emit8(0xc7); emit8(0x05);         // mov [heap offset], saved
            emit32(static_cast<std::uint32_t>(address(generatedHeapOffset)));
            emit32(savedGeneratedHeapOffset);
        }
        emit8(0x61);                         // popad
        emit8(0x9d);                         // popfd
        const auto returnOffset = backendCompletionMode
                == BackendCompletionMode::zero
            ? backendTransportReadbackOffset
            : backendTransportEpilogueOffset;
        emit8(0x68); emit32(static_cast<std::uint32_t>(address(returnOffset)));
        emit8(0xc3);                         // ret

        std::memcpy(reinterpret_cast<void*>(workspaceBase + transactionThunkOffset),
                    code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<void*>(workspaceBase + transactionThunkOffset),
                              code.size());
    }

    void initializeRenderDescriptor(std::uint32_t frames = 512,
                                    std::uint32_t frameOffset = 0)
    {
        auto* descriptor = reinterpret_cast<std::uint32_t*>(
            workspaceBase + descriptorOffset);
        descriptor[0] = 0;
        descriptor[1] = frames;
        descriptor[2] = 0;
        descriptor[3] = static_cast<std::uint32_t>(
            renderBuffer() + frameOffset * sizeof(std::int16_t) * 2);
        descriptor[4] = 0;
    }

    void warmUpRenderer()
    {
        initializeRenderDescriptor();
        write32(address(backendGateOffset), 0);
        call2(dispatcher(), 7,
              static_cast<std::uint32_t>(workspaceBase + descriptorOffset));
        if (renderPath == RenderPath::transport)
            normalizeGeneratedAllocators();
        rendererWarm = true;
    }

    void captureAndRender()
    {
        if (!rendererWarm)
            warmUpRenderer();
        capturedRendererByteCount = 0;
        capturedRendererTransactionCount = 0;
        captureCompleted = false;
        savedGeneratedHeapOffset = read32(address(generatedHeapOffset));
        serviceEntryRingPhase = read32(address(0x0016d7bc));
        captureActive.store(true);
        write32(address(backendGateOffset), 1);
        call2(dispatcher(), 7,
              static_cast<std::uint32_t>(workspaceBase + descriptorOffset));
        captureActive.store(false);
        if (!codeDumpCompleted) {
            dumpGeneratedCode(
                0, rendererCommand, diagnosticSampleCallback,
                static_cast<std::uint32_t>(
                    address(backendTransportReadbackOffset - 0x100)));
            codeDumpCompleted = true;
        }
        const auto expectedBytes = capturedRendererTransactionCount
            * captureOffsets.size();
        if (!captureCompleted || capturedRendererByteCount != expectedBytes) {
            char message[160] {};
            std::snprintf(message, sizeof(message),
                          "PVL backend did not complete a renderer transaction "
                          "(gate=%08lx available=%08lx transactions=%lu bytes=%lu)",
                          static_cast<unsigned long>(read32(address(backendGateOffset))),
                          static_cast<unsigned long>(read32(address(backendAvailableOffset))),
                          static_cast<unsigned long>(capturedRendererTransactionCount),
                          static_cast<unsigned long>(capturedRendererByteCount));
            throw std::runtime_error(message);
        }
    }

    [[nodiscard]] std::uintptr_t address(std::uintptr_t offset) const
    {
        return imageBase + offset;
    }
    [[nodiscard]] std::uintptr_t dispatcher() const
    {
        return address(commandDispatcherOffset);
    }
    [[nodiscard]] std::uintptr_t pointerTable() const
    {
        return workspaceBase + pointerTableOffset;
    }
    [[nodiscard]] std::uintptr_t renderBuffer() const
    {
        return workspaceBase + renderBufferOffset;
    }

    std::uintptr_t imageBase {};
    std::uintptr_t workspaceBase {};
    HANDLE callbackThread {};
    std::atomic<bool> callbackReady {};
    std::atomic<bool> stopCallback {};
    std::atomic<bool> captureActive {};
    std::array<std::uint8_t, 4> capturedRendererBytes {};
    std::size_t capturedRendererByteCount {};
    std::size_t capturedRendererTransactionCount {};
    std::uint32_t transactionLogCount {};
    bool captureCompleted {};
    std::uint32_t rendererCommand {};
    std::uint32_t diagnosticSampleCallback {};
    std::uint32_t lastDescriptorFrames {};
    std::uint32_t lastTransactionFrames {};
    std::array<std::uint32_t, 4> lastTransportArguments {};
    std::uint32_t savedGeneratedHeapOffset {};
    std::uint32_t serviceEntryRingPhase {};
    GeneratedHeapMode generatedHeapMode { GeneratedHeapMode::restore };
    GeneratedJitterMode generatedJitterMode { GeneratedJitterMode::zero };
    BackendCompletionMode backendCompletionMode { BackendCompletionMode::skip };
    RenderPath renderPath { RenderPath::direct };
    std::uint32_t nativeRenderQuantum { defaultNativeRenderQuantum };
    bool rendererWarm {};
    bool codeDumpCompleted {};
    bool deviceInitialized {};
    bool callbackSuspended {};
    inline static thread_local Impl* activeEngine {};
};

NativeVlEngine::NativeVlEngine(const std::filesystem::path& vxdPath,
                               std::uint32_t sampleRate)
    : impl(std::make_unique<Impl>(vxdPath, sampleRate))
{
}

NativeVlEngine::~NativeVlEngine() = default;

void NativeVlEngine::sendShort(std::uint32_t packedMessage)
{
    impl->sendShort(packedMessage);
}

void NativeVlEngine::sendSysex(std::span<const std::uint8_t> bytes)
{
    impl->sendSysex(bytes);
}

void NativeVlEngine::setSampleRate(std::uint32_t sampleRate)
{
    impl->setSampleRate(sampleRate);
}

void NativeVlEngine::warmUp()
{
    impl->warmUp();
}

void NativeVlEngine::prepare()
{
    impl->prepare();
}

void NativeVlEngine::render(std::uint32_t frames)
{
    impl->render(frames);
}

std::span<const std::int16_t> NativeVlEngine::plane(
    std::size_t index, std::uint32_t frames) const
{
    return impl->plane(index, frames);
}

} // namespace hybrid
