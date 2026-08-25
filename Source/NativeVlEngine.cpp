#include "NativeVlEngine.h"

#include "LeImageLoader.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csetjmp>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace hybrid {
namespace {

constexpr std::size_t imageSize = 0x001aecdc;
constexpr std::size_t imageReservationStep = 0x00200000;
constexpr std::uintptr_t firstImageBase = 0x10000000;
constexpr std::uintptr_t lastImageBase = 0x60000000;
constexpr std::size_t workspaceSize = 0x00100000;
constexpr std::uintptr_t firstWorkspaceBase = 0x00500000;
constexpr std::uintptr_t lastWorkspaceBase = 0x08000000;
constexpr std::size_t descriptorOffset = 0x00030000;
constexpr std::size_t pointerTableOffset = 0x0003f000;
constexpr std::size_t renderBufferOffset = 0x00040000;
constexpr std::size_t planeStride = 0x2000;
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
constexpr std::array<std::uintptr_t, 4> captureOffsets {
    0x001a6e13, 0x001a6e17, 0x001a6e1b, 0x001a6e1f,
};

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

} // namespace

class NativeVlEngine::Impl {
public:
    Impl(const std::filesystem::path& vxdPath, std::uint32_t sampleRate)
    {
        try {
            installExceptionRouter();
            allocateImage(vxdPath);
            allocateWorkspace();
            callbackReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            stopCallback = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (callbackReady == nullptr || stopCallback == nullptr)
                throw std::runtime_error("cannot create VL callback events");

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
            if (WaitForSingleObject(callbackReady, 10'000) != WAIT_OBJECT_0)
                throw std::runtime_error("VL callback initialization timed out");
            enableCallbackStackCode();
            call0(address(callbackInstallOffset));
            call2(dispatcher(), 13, 8);
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
        if (stopCallback != nullptr)
            SetEvent(stopCallback);
        if (callbackThread != nullptr) {
            WaitForSingleObject(callbackThread, 5'000);
            CloseHandle(callbackThread);
        }
        if (callbackReady != nullptr)
            CloseHandle(callbackReady);
        if (stopCallback != nullptr)
            CloseHandle(stopCallback);
        if (workspaceBase != 0)
            VirtualFree(reinterpret_cast<void*>(workspaceBase), 0, MEM_RELEASE);
        if (imageBase != 0)
            VirtualFree(reinterpret_cast<void*>(imageBase), 0, MEM_RELEASE);
        callbackThread = nullptr;
        callbackReady = nullptr;
        stopCallback = nullptr;
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
        if (rendererCommand != 0)
            return;
        ActiveScope scope(this);
        prepareRenderer();
    }

    void render(std::uint32_t frames)
    {
        if (frames == 0 || frames > NativeVlEngine::maxFrames)
            throw std::runtime_error("invalid VL render block size");
        ActiveScope scope(this);
        if (rendererCommand == 0)
            prepareRenderer();
        std::memset(reinterpret_cast<void*>(renderBuffer()), 0,
                    planeStride * NativeVlEngine::planeCount);
        write32(address(backendGateOffset), 0);
        call3(rendererCommand, static_cast<std::uint32_t>(pointerTable()), 0,
              frames);
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
        if (record->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
            return EXCEPTION_CONTINUE_SEARCH;

        if (instruction[0] == 0xec || instruction[0] == 0xed
            || instruction[0] == 0xe4 || instruction[0] == 0xe5) {
            const auto status = captureActive.load() ? 5u : 0u;
            context->Eax = (context->Eax & 0xffffff00u) | status;
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
                if (index + 1 == captureOffsets.size() && captureActive.load())
                    std::longjmp(captureJump, 1);
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
        SetEvent(self->callbackReady);
        WaitForSingleObject(self->stopCallback, INFINITE);
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

    void prepareRenderer()
    {
        auto* descriptor = reinterpret_cast<std::uint32_t*>(
            workspaceBase + descriptorOffset);
        descriptor[0] = 0;
        descriptor[1] = 512;
        descriptor[2] = 0;
        descriptor[3] = static_cast<std::uint32_t>(renderBuffer());
        descriptor[4] = 0;
        write32(address(backendGateOffset), 0);
        call2(dispatcher(), 7,
              static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(descriptor)));

        capturedRendererByteCount = 0;
        captureActive.store(true);
        write32(address(backendGateOffset), 1);
        if (setjmp(captureJump) == 0) {
            call2(dispatcher(), 7,
                  static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(
                      descriptor)));
            captureActive.store(false);
            throw std::runtime_error("PVL backend did not submit a renderer");
        }
        captureActive.store(false);
        if (capturedRendererByteCount != captureOffsets.size())
            throw std::runtime_error("incomplete PVL renderer command");
        std::memcpy(&rendererCommand, capturedRendererBytes.data(),
                    sizeof(rendererCommand));
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
    HANDLE callbackReady {};
    HANDLE stopCallback {};
    HANDLE callbackThread {};
    std::atomic<bool> captureActive {};
    std::array<std::uint8_t, 4> capturedRendererBytes {};
    std::size_t capturedRendererByteCount {};
    std::jmp_buf captureJump;
    std::uint32_t rendererCommand {};
    bool deviceInitialized {};
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
