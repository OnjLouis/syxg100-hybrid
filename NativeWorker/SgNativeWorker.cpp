#include "../Source/NativeSgEngine.h"
#include "../Source/NativeVlProtocol.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

namespace {

HANDLE parseHandle(const wchar_t* text)
{
    return reinterpret_cast<HANDLE>(_wcstoui64(text, nullptr, 10));
}

void setDiagnostic(hybrid::ipc::SharedState& shared, const char* text)
{
    const auto length = std::min(std::strlen(text), shared.diagnostic.size());
    std::copy_n(text, length, shared.diagnostic.begin());
    shared.diagnosticSize = static_cast<std::uint32_t>(length);
}

struct WorkerStatistics {
    std::uint64_t shortMessages {};
    std::uint64_t noteOns {};
    std::uint64_t acceptedNoteOns {};
    std::uint64_t sysexMessages {};
    std::uint64_t renderedFrames {};
    std::uint32_t peakSample {};
};

void writeStatistics(const WorkerStatistics& statistics)
{
    wchar_t path[MAX_PATH] {};
    const auto pathLength = GetEnvironmentVariableW(
        L"SYXG100_SG_WORKER_LOG", path, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH)
        return;
    char text[320] {};
    const auto length = std::snprintf(
        text, sizeof(text),
        "short=%llu note-ons=%llu accepted-note-ons=%llu sysex=%llu "
        "rendered-frames=%llu peak-sample=%lu\n",
        static_cast<unsigned long long>(statistics.shortMessages),
        static_cast<unsigned long long>(statistics.noteOns),
        static_cast<unsigned long long>(statistics.acceptedNoteOns),
        static_cast<unsigned long long>(statistics.sysexMessages),
        static_cast<unsigned long long>(statistics.renderedFrames),
        static_cast<unsigned long>(statistics.peakSample));
    const auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
                                  nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written {};
    WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 6)
        return 64;
    const auto mapping = parseHandle(argv[1]);
    const auto requestEvent = parseHandle(argv[2]);
    const auto responseEvent = parseHandle(argv[3]);
    const auto sampleRate = static_cast<std::uint32_t>(
        _wcstoui64(argv[4], nullptr, 10));
    auto* shared = static_cast<hybrid::ipc::SharedState*>(MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0,
        sizeof(hybrid::ipc::SharedState)));
    if (shared == nullptr)
        return 65;

    std::unique_ptr<hybrid::NativeSgEngine> engine;
    try {
        engine = std::make_unique<hybrid::NativeSgEngine>(argv[5], sampleRate);
        InterlockedExchange(&shared->result, 0);
    } catch (const std::exception& error) {
        setDiagnostic(*shared, error.what());
        InterlockedExchange(&shared->result, 1);
        SetEvent(responseEvent);
        UnmapViewOfFile(shared);
        return 1;
    } catch (...) {
        setDiagnostic(*shared, "unknown native SG initialization failure");
        InterlockedExchange(&shared->result, 1);
        SetEvent(responseEvent);
        UnmapViewOfFile(shared);
        return 1;
    }
    SetEvent(responseEvent);

    bool running = true;
    WorkerStatistics statistics;
    wchar_t statisticsPath[MAX_PATH] {};
    const bool statisticsEnabled = GetEnvironmentVariableW(
        L"SYXG100_SG_WORKER_LOG", statisticsPath,
        static_cast<DWORD>(std::size(statisticsPath))) != 0;
    while (running
           && WaitForSingleObject(requestEvent, INFINITE) == WAIT_OBJECT_0) {
        try {
            shared->diagnosticSize = 0;
            const auto command = static_cast<hybrid::ipc::Command>(
                InterlockedExchange(&shared->command, 0));
            switch (command) {
            case hybrid::ipc::Command::sendShort: {
                if (statisticsEnabled)
                    ++statistics.shortMessages;
                const auto operation = shared->argument & 0xf0;
                const auto velocity = (shared->argument >> 16) & 0x7f;
                if (statisticsEnabled && operation == 0x90 && velocity != 0) {
                    ++statistics.noteOns;
                    const auto channel = shared->argument & 0x0f;
                    statistics.acceptedNoteOns +=
                        (engine->routeMask()
                         & (std::uint32_t {1} << channel)) != 0;
                }
                engine->sendShort(shared->argument);
                break;
            }
            case hybrid::ipc::Command::sendSysex:
                if (shared->dataSize > shared->data.size())
                    throw std::runtime_error("SG SysEx exceeds IPC capacity");
                engine->sendSysex({shared->data.data(), shared->dataSize});
                if (statisticsEnabled)
                    ++statistics.sysexMessages;
                break;
            case hybrid::ipc::Command::setSampleRate:
                engine->setSampleRate(shared->argument);
                break;
            case hybrid::ipc::Command::render: {
                engine->render(shared->argument);
                if (statisticsEnabled)
                    statistics.renderedFrames += shared->argument;
                for (std::size_t index = 0;
                     index < hybrid::ipc::planeCount; ++index) {
                    const auto audio = engine->plane(index, shared->argument);
                    std::copy(audio.begin(), audio.end(),
                              shared->audio.begin()
                                  + index * hybrid::ipc::maxStereoSamples);
                    if (statisticsEnabled) {
                        for (const auto sample : audio) {
                            const auto magnitude = sample == INT16_MIN
                                ? std::uint32_t {32768}
                                : static_cast<std::uint32_t>(std::abs(sample));
                            statistics.peakSample = std::max(
                                statistics.peakSample, magnitude);
                        }
                    }
                }
                break;
            }
            case hybrid::ipc::Command::getRouteMask:
                shared->argument = engine->routeMask();
                break;
            case hybrid::ipc::Command::shutdown:
                running = false;
                break;
            default:
                throw std::runtime_error("unsupported native SG command");
            }
            InterlockedExchange(&shared->result, 0);
        } catch (const std::exception& error) {
            setDiagnostic(*shared, error.what());
            InterlockedExchange(&shared->result, 1);
        } catch (...) {
            setDiagnostic(*shared, "unknown native SG worker failure");
            InterlockedExchange(&shared->result, 1);
        }
        SetEvent(responseEvent);
    }
    if (statisticsEnabled)
        writeStatistics(statistics);
    engine.reset();
    UnmapViewOfFile(shared);
    return 0;
}
