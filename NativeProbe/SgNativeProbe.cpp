#include "../Source/LeImageLoader.h"

#include <windows.h>

#include <algorithm>
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
constexpr std::uintptr_t fpuGuardStartOffset = 0x143573;
constexpr std::uintptr_t fpuGuardEndOffset = 0x1435df;
constexpr std::uintptr_t ringCliOffset = 0x1422ef;
constexpr std::uintptr_t ringStiOffset = 0x14230e;
constexpr std::uint32_t vmmHeapAllocate = 0x0001804f;
constexpr std::uint32_t vmmHeapFree = 0x00018051;
std::size_t allocationCount {};
std::size_t allocatedBytes {};
std::size_t freeCount {};

LONG WINAPI reportException(EXCEPTION_POINTERS* details)
{
    const auto* record = details->ExceptionRecord;
    auto* context = details->ContextRecord;
    const auto* instruction = reinterpret_cast<const std::uint8_t*>(context->Eip);
    const auto inFpuGuard = context->Eip >= imageBase + fpuGuardStartOffset
        && context->Eip < imageBase + fpuGuardEndOffset;
    const auto isRingInterruptGuard = context->Eip == imageBase + ringCliOffset
        || context->Eip == imageBase + ringStiOffset;
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

void renderFrames(std::uintptr_t tick, std::uint32_t frames,
                  std::vector<std::int16_t>& rendered)
{
    // The original SG renderer's fixed scratch area permits at most 388
    // frames; exceeding it overwrites adjacent kernel globals.
    constexpr std::uint32_t blockSize = 256;
    std::vector<std::int16_t> block(blockSize);
    while (frames != 0) {
        const auto count = std::min(frames, blockSize);
        callNoArguments(imageBase + processEventsOffset);
        callTick(tick, block.data(), count, 1.0f);
        rendered.insert(rendered.end(), block.begin(), block.begin() + count);
        frames -= count;
    }
}

void replayTrace(std::uintptr_t dispatcher, std::uintptr_t tick,
                 const std::filesystem::path& path,
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
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::uint32_t delta = 0;
        char kind = 0;
        std::string hexadecimal;
        if (!(fields >> delta >> kind >> hexadecimal))
            continue;
        renderFrames(tick, delta, rendered);
        const auto bytes = parseHex(hexadecimal);
        if (kind == 'M') {
            std::uint32_t packed = 0;
            if (bytes.empty() || bytes.size() > sizeof(packed))
                throw std::runtime_error("invalid short MIDI message");
            std::copy(bytes.begin(), bytes.end(),
                      reinterpret_cast<std::uint8_t*>(&packed));
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
        ++events;
    }
    renderFrames(tick, 44'100, rendered);
    std::printf("replayed SG events=%zu rendered frames=%zu\n", events,
                rendered.size());
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
        const auto configureResult = callSg(imageBase + sgDispatcherOffset,
                                            5, 0, 0);
        std::printf("SG configure returned 0x%08x\n", configureResult);
        const auto openResult = callUnary(imageBase + vfmOpenOffset, 44'100);
        std::printf("vfmOpen returned 0x%08x\n", openResult);
        std::printf("MIDI parser after vfmOpen=0x%08x\n", *parserPointer);
        callSg(imageBase + sgDispatcherOffset, 17, 5, 0);
        const auto messageOpen = callSg(imageBase + sgDispatcherOffset,
                                        17, 0x20, 0);
        std::printf("MIDI parser after message open=0x%08x\n", *parserPointer);
        std::vector<std::int16_t> output;
        std::uint32_t tickResult = 0;
        if (argc >= 3) {
            replayTrace(imageBase + sgDispatcherOffset,
                        imageBase + vfmTickOffset, argv[2], output);
        } else {
            const auto noteOn = callSg(imageBase + sgDispatcherOffset,
                                       17, 0x60, 0x00643c9b);
            std::printf("SG message open=0x%08x note-on=0x%08x\n",
                        messageOpen, noteOn);
            output.resize(512, -1);
            callNoArguments(imageBase + processEventsOffset);
            tickResult = callTick(imageBase + vfmTickOffset,
                                  output.data(), output.size(), 1.0f);
            callSg(imageBase + sgDispatcherOffset, 17, 0x60, 0x00003c8b);
        }
        const auto nonzero = std::count_if(output.begin(), output.end(),
                                           [](auto sample) { return sample != 0; });
        std::printf("vfmTick returned 0x%08x nonzero=%zu/%zu\n", tickResult,
                    nonzero, output.size());
        if (argc == 4)
            writeWave(argv[3], output);
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
