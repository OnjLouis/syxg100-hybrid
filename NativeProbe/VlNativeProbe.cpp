#include <windows.h>

#include <array>
#include <csetjmp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uintptr_t imageBase = 0x10000000;
constexpr std::size_t imageSize = 0x001aecdc;
constexpr std::uintptr_t criticalInit = imageBase + 0x00172100;
constexpr std::uintptr_t deviceInit = imageBase + 0x00172110;
constexpr std::uintptr_t commandDispatcher = imageBase + 0x001721b0;

volatile LONG skippedCli = 0;
volatile LONG skippedSti = 0;
volatile LONG skippedCr0Read = 0;
volatile LONG skippedCr0Write = 0;
volatile LONG skippedClts = 0;
volatile LONG enabledGeneratedStackPages = 0;
volatile LONG callbackReady = 0;
HANDLE callbackThread = nullptr;
std::array<std::uint8_t, 4> capturedRenderBytes {};
volatile LONG capturedRenderByteCount = 0;
volatile LONG captureActive = 0;
std::jmp_buf captureJump;

LONG WINAPI reportException(EXCEPTION_POINTERS* details)
{
    const auto* record = details->ExceptionRecord;
    auto* context = details->ContextRecord;
    const auto* instruction = reinterpret_cast<const std::uint8_t*>(context->Eip);
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
        if (VirtualProtect(page, pageSize, PAGE_EXECUTE_READWRITE, &oldProtection)) {
            InterlockedIncrement(&enabledGeneratedStackPages);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (record->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION) {
        if (instruction[0] == 0xec || instruction[0] == 0xed
            || instruction[0] == 0xe4 || instruction[0] == 0xe5) {
            const auto backendStatus =
                InterlockedCompareExchange(&captureActive, 0, 0) != 0 ? 5u : 0u;
            context->Eax = (context->Eax & 0xffffff00u) | backendStatus;
            context->Eip += (instruction[0] == 0xe4 || instruction[0] == 0xe5) ? 2 : 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0xee || instruction[0] == 0xef
            || instruction[0] == 0xe6 || instruction[0] == 0xe7) {
            const auto offset = context->Eip - imageBase;
            constexpr std::array<std::uintptr_t, 4> captureOffsets {
                0x001a6e13, 0x001a6e17, 0x001a6e1b, 0x001a6e1f,
            };
            for (std::size_t index = 0; index < captureOffsets.size(); ++index) {
                if (offset == captureOffsets[index]) {
                    capturedRenderBytes[index] = static_cast<std::uint8_t>(context->Eax);
                    InterlockedIncrement(&capturedRenderByteCount);
                    if (index == captureOffsets.size() - 1
                        && InterlockedCompareExchange(&captureActive, 0, 0) != 0)
                        std::longjmp(captureJump, 1);
                    break;
                }
            }
            context->Eip += (instruction[0] == 0xe6 || instruction[0] == 0xe7) ? 2 : 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0xfa) {
            InterlockedIncrement(&skippedCli);
            context->Eip += 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0xfb) {
            InterlockedIncrement(&skippedSti);
            context->Eip += 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0x0f && instruction[1] == 0x20
            && instruction[2] == 0xc0) {
            InterlockedIncrement(&skippedCr0Read);
            context->Eax = 0;
            context->Eip += 3;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0x0f && instruction[1] == 0x22
            && instruction[2] == 0xc0) {
            InterlockedIncrement(&skippedCr0Write);
            context->Eip += 3;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (instruction[0] == 0x0f && instruction[1] == 0x06) {
            InterlockedIncrement(&skippedClts);
            context->Eip += 2;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    std::cerr << "exception code=0x" << std::hex << std::setw(8)
              << std::setfill('0') << record->ExceptionCode
              << " address=0x" << reinterpret_cast<std::uintptr_t>(record->ExceptionAddress)
              << " eip=0x" << context->Eip
              << " eax=0x" << context->Eax
              << " ebx=0x" << context->Ebx
              << " ecx=0x" << context->Ecx
              << " edx=0x" << context->Edx
              << " esi=0x" << context->Esi
              << " edi=0x" << context->Edi
              << " ebp=0x" << context->Ebp
              << " esp=0x" << context->Esp << '\n';
    MEMORY_BASIC_INFORMATION region {};
    if (VirtualQuery(instruction, &region, sizeof(region)) != 0
        && region.State == MEM_COMMIT) {
        std::cerr << "region base=0x"
                  << reinterpret_cast<std::uintptr_t>(region.BaseAddress)
                  << " allocation=0x"
                  << reinterpret_cast<std::uintptr_t>(region.AllocationBase)
                  << " size=0x" << region.RegionSize
                  << " protect=0x" << region.Protect
                  << " type=0x" << region.Type << '\n';
        std::cerr << "instruction bytes=";
        for (std::size_t index = 0; index < 12; ++index)
            std::cerr << ' ' << std::setw(2) << static_cast<unsigned>(instruction[index]);
        std::cerr << '\n';
    }
    const auto* stack = reinterpret_cast<const std::uint32_t*>(context->Esp);
    std::cerr << "stack=";
    for (std::size_t index = 0; index < 16; ++index)
        std::cerr << ' ' << std::setw(8) << stack[index];
    std::cerr << '\n';
    std::cerr.flush();
    ExitProcess(2);
}

std::vector<std::uint8_t> readImage(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("cannot open " + path.string());
    const auto size = input.tellg();
    if (size != static_cast<std::streamoff>(imageSize))
        throw std::runtime_error("image must be exactly 0x1aecdc bytes");
    std::vector<std::uint8_t> bytes(imageSize);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input)
        throw std::runtime_error("cannot read " + path.string());
    return bytes;
}

std::vector<std::uint8_t> parseHex(std::string_view text)
{
    if ((text.size() % 2) != 0)
        throw std::runtime_error("odd hexadecimal event length");
    std::vector<std::uint8_t> bytes(text.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(
            std::stoul(std::string(text.substr(index * 2, 2)), nullptr, 16));
    return bytes;
}

std::uint32_t call0(std::uintptr_t address)
{
    const auto function = reinterpret_cast<std::uint32_t(__cdecl*)()>(address);
    return function();
}

std::uint32_t call1(std::uintptr_t address, std::uint32_t first)
{
    const auto function = reinterpret_cast<std::uint32_t(__cdecl*)(std::uint32_t)>(address);
    return function(first);
}

std::uint32_t call2(std::uintptr_t address, std::uint32_t first,
                    std::uint32_t second)
{
    const auto function = reinterpret_cast<std::uint32_t(__cdecl*)(
        std::uint32_t, std::uint32_t)>(address);
    return function(first, second);
}

std::uint32_t call3(std::uintptr_t address, std::uint32_t first,
                    std::uint32_t second, std::uint32_t third)
{
    const auto function = reinterpret_cast<std::uint32_t(__cdecl*)(
        std::uint32_t, std::uint32_t, std::uint32_t)>(address);
    return function(first, second, third);
}

void write32(std::uintptr_t address, std::uint32_t value)
{
    *reinterpret_cast<volatile std::uint32_t*>(address) = value;
}

std::uint32_t read32(std::uintptr_t address)
{
    return *reinterpret_cast<volatile const std::uint32_t*>(address);
}

DWORD WINAPI generateCallbackOnRetainedStack(void*)
{
    write32(imageBase + 0x000e4200,
            static_cast<std::uint32_t>(imageBase + 0x00095ee0));
    call0(imageBase + 0x00197620);
    InterlockedExchange(&callbackReady, 1);
    for (;;)
        YieldProcessor();
}

void stabiliseRuntimeCallback()
{
    callbackThread = CreateThread(nullptr, 0, generateCallbackOnRetainedStack,
                                  nullptr, 0, nullptr);
    if (callbackThread == nullptr)
        throw std::runtime_error("could not create callback thread");
    while (InterlockedCompareExchange(&callbackReady, 0, 0) == 0)
        Sleep(1);
    const auto callback = read32(imageBase + 0x000f1f60);
    constexpr std::uintptr_t pageMask = 0xfff;
    const auto firstPage = (callback - 0x10000) & ~pageMask;
    const auto lastPage = (callback + 0x10000) & ~pageMask;
    for (auto page = firstPage; page <= lastPage; page += 0x1000) {
        MEMORY_BASIC_INFORMATION region {};
        if (VirtualQuery(reinterpret_cast<void*>(page), &region, sizeof(region)) == 0
            || region.State != MEM_COMMIT
            || (region.Protect & PAGE_GUARD) != 0)
            continue;
        DWORD oldProtection = 0;
        VirtualProtect(reinterpret_cast<void*>(page), 0x1000,
                       PAGE_EXECUTE_READWRITE, &oldProtection);
    }
    std::cout << "callback retained at 0x" << std::hex << callback
              << std::dec << '\n';
    call0(imageBase + 0x00195730);
}

void sendShort(std::span<const std::uint8_t> bytes)
{
    std::uint32_t packed = 0;
    if (bytes.empty() || bytes.size() > sizeof(packed))
        throw std::runtime_error("invalid short MIDI message");
    std::copy(bytes.begin(), bytes.end(), reinterpret_cast<std::uint8_t*>(&packed));
    call2(commandDispatcher, 5, packed);
}

void sendSysex(std::span<const std::uint8_t> bytes)
{
    const std::array<std::uint32_t, 2> header {
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(bytes.data())),
        static_cast<std::uint32_t>(bytes.size()),
    };
    call2(commandDispatcher, 6,
          static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(header.data())));
}

void replayTrace(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open " + path.string());
    std::string line;
    if (!std::getline(input, line) || line != "PVTE 1")
        throw std::runtime_error("unsupported event trace");
    std::size_t events = 0;
    while (std::getline(input, line)) {
        if (line.size() < 3 || line[1] != ' ')
            continue;
        const auto bytes = parseHex(std::string_view(line).substr(2));
        if (line[0] == 'M')
            sendShort(bytes);
        else if (line[0] == 'S')
            sendSysex(bytes);
        else
            throw std::runtime_error("unknown event trace record");
        ++events;
    }
    std::cout << "replayed events=" << events << '\n';
}

void printStats(const std::array<std::vector<std::int16_t>, 4>& planes)
{
    for (std::size_t plane = 0; plane < planes.size(); ++plane) {
        std::size_t nonzero = 0;
        int peak = 0;
        for (const auto sample : planes[plane]) {
            nonzero += sample != 0;
            peak = std::max(peak, std::abs(static_cast<int>(sample)));
        }
        std::cout << " plane" << (plane + 1) << '=' << nonzero << '/' << peak;
    }
    std::cout << '\n';
}

std::array<std::vector<std::int16_t>, 4> renderDirectFixed(
    std::uint32_t command, std::uint32_t frames)
{
    constexpr std::size_t dataSize = 0x00100000;
    constexpr std::size_t planeStride = 0x2000;
    if (frames * sizeof(std::int16_t) * 2 > planeStride)
        throw std::runtime_error("fixed render block exceeds plane stride");
    std::uintptr_t dataBase = 0;
    for (std::uintptr_t candidate = 0x00500000; candidate < 0x08000000;
         candidate += dataSize) {
        const auto workspace = VirtualAlloc(reinterpret_cast<void*>(candidate), dataSize,
                                            MEM_RESERVE | MEM_COMMIT,
                                            PAGE_READWRITE);
        if (workspace == reinterpret_cast<void*>(candidate)) {
            dataBase = candidate;
            break;
        }
    }
    if (dataBase == 0)
        throw std::runtime_error("cannot allocate fixed renderer workspace");
    const auto pointerTable = dataBase + 0x0003f000;
    const auto renderBuffer = dataBase + 0x00040000;
    std::memset(reinterpret_cast<void*>(dataBase), 0, dataSize);
    std::array<std::uint32_t, 4> addresses {};
    for (std::size_t index = 0; index < addresses.size(); ++index)
        addresses[index] = static_cast<std::uint32_t>(renderBuffer
                                                       + index * planeStride);
    std::memcpy(reinterpret_cast<void*>(pointerTable), addresses.data(),
                sizeof(addresses));
    write32(imageBase + 0x00159ee0, 0);
    call3(command, static_cast<std::uint32_t>(pointerTable), 0, frames);
    std::array<std::vector<std::int16_t>, 4> planes;
    for (std::size_t index = 0; index < planes.size(); ++index) {
        planes[index].resize(frames * 2);
        std::memcpy(planes[index].data(),
                    reinterpret_cast<const void*>(addresses[index]),
                    frames * sizeof(std::int16_t) * 2);
    }
    return planes;
}

std::uint32_t captureDirectRenderer(std::uint32_t frames)
{
    std::vector<std::uint8_t> buffer(0x20000);
    const std::array<std::uint32_t, 5> descriptor {
        0, frames, 0,
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(buffer.data())),
        0,
    };
    capturedRenderByteCount = 0;
    write32(imageBase + 0x00159ee0, 1);
    if (setjmp(captureJump) == 0) {
        InterlockedExchange(&captureActive, 1);
        call2(commandDispatcher, 7,
              static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(descriptor.data())));
        InterlockedExchange(&captureActive, 0);
        throw std::runtime_error("backend capture returned before four command bytes");
    }
    InterlockedExchange(&captureActive, 0);
    std::uint32_t command = 0;
    std::copy(capturedRenderBytes.begin(), capturedRenderBytes.end(),
              reinterpret_cast<std::uint8_t*>(&command));
    return command;
}

std::uint32_t prepareDirectRenderer()
{
    constexpr std::uint32_t frames = 512;
    std::vector<std::uint8_t> buffer(0x20000);
    const std::array<std::uint32_t, 5> descriptor {
        0, frames, 0,
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(buffer.data())),
        0,
    };
    write32(imageBase + 0x00159ee0, 0);
    call2(commandDispatcher, 7,
          static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(descriptor.data())));
    return captureDirectRenderer(frames);
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 3) {
            std::cerr << "usage: VlNativeProbe <relocated-pv-image.bin> "
                         "<events.pvte.txt>\n";
            return 64;
        }

        const auto image = readImage(argv[1]);
        auto* allocation = VirtualAlloc(reinterpret_cast<void*>(imageBase), imageSize,
                                        MEM_RESERVE | MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE);
        if (allocation != reinterpret_cast<void*>(imageBase))
            throw std::runtime_error("could not reserve the required image base");
        std::copy(image.begin(), image.end(), static_cast<std::uint8_t*>(allocation));
        FlushInstructionCache(GetCurrentProcess(), allocation, imageSize);
        AddVectoredExceptionHandler(1, reportException);

        std::cout << "mapped image at 0x" << std::hex << imageBase << '\n';
        std::cout << "critical init returned 0x" << call0(criticalInit) << '\n';
        std::cout << "device init returned 0x" << call0(deviceInit) << '\n';
        stabiliseRuntimeCallback();
        std::cout << "runtime callback stabilised\n";
        std::cout << "setting engine mode\n";
        call2(commandDispatcher, 13, 8);
        std::cout << "initialising render state\n";
        call0(imageBase + 0x0016d9c1);
        std::cout << "setting sample rate\n";
        call3(commandDispatcher, 4, 44'100, 44'100);
        std::cout << "replaying trace\n";
        replayTrace(argv[2]);
        auto capturedCommand = prepareDirectRenderer();
        std::cout << "captured render command=0x" << std::hex << capturedCommand
                  << std::dec << " bytes=" << capturedRenderByteCount << '\n';

        const std::array<std::uint8_t, 3> retrigger { 0x90, 58, 100 };
        sendShort(retrigger);
        constexpr std::uint32_t frames = 2048;
        const auto renderStart = std::chrono::steady_clock::now();
        const auto planes = renderDirectFixed(capturedCommand, frames);
        const auto renderMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - renderStart).count();
        std::cout << "fixed direct render milliseconds=" << renderMilliseconds;
        printStats(planes);
        std::cout << std::dec
                  << "compatibility skips: cli=" << skippedCli
                  << " sti=" << skippedSti
                  << " cr0-read=" << skippedCr0Read
                  << " cr0-write=" << skippedCr0Write
                  << " clts=" << skippedClts
                  << " generated-stack-pages=" << enabledGeneratedStackPages << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
