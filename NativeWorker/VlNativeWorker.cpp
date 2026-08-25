#include "../Source/NativeVlEngine.h"
#include "../Source/NativeVlProtocol.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace {

HANDLE parseHandle(const wchar_t* text)
{
    return reinterpret_cast<HANDLE>(_wcstoui64(text, nullptr, 10));
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 6)
        return 64;
    const auto mapping = parseHandle(argv[1]);
    const auto requestEvent = parseHandle(argv[2]);
    const auto responseEvent = parseHandle(argv[3]);
    const auto sampleRate = static_cast<std::uint32_t>(_wcstoui64(argv[4], nullptr, 10));
    auto* shared = static_cast<hybrid::ipc::SharedState*>(MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hybrid::ipc::SharedState)));
    if (shared == nullptr)
        return 65;

    std::unique_ptr<hybrid::NativeVlEngine> engine;
    try {
        engine = std::make_unique<hybrid::NativeVlEngine>(argv[5], sampleRate);
        InterlockedExchange(&shared->result, 0);
    } catch (...) {
        InterlockedExchange(&shared->result, 1);
        SetEvent(responseEvent);
        UnmapViewOfFile(shared);
        return 1;
    }
    SetEvent(responseEvent);

    bool running = true;
    while (running && WaitForSingleObject(requestEvent, INFINITE) == WAIT_OBJECT_0) {
        try {
            const auto command = static_cast<hybrid::ipc::Command>(
                InterlockedExchange(&shared->command, 0));
            switch (command) {
            case hybrid::ipc::Command::sendShort:
                engine->sendShort(shared->argument);
                break;
            case hybrid::ipc::Command::sendSysex:
                if (shared->dataSize > shared->data.size())
                    throw 1;
                engine->sendSysex({ shared->data.data(), shared->dataSize });
                break;
            case hybrid::ipc::Command::setSampleRate:
                engine->setSampleRate(shared->argument);
                break;
            case hybrid::ipc::Command::prepare:
                engine->prepare();
                break;
            case hybrid::ipc::Command::render: {
                engine->render(shared->argument);
                for (std::size_t index = 0;
                     index < hybrid::ipc::planeCount; ++index) {
                    const auto audio = engine->plane(index, shared->argument);
                    std::copy(audio.begin(), audio.end(),
                              shared->audio.begin()
                                  + index * hybrid::ipc::maxStereoSamples);
                }
                break;
            }
            case hybrid::ipc::Command::shutdown:
                running = false;
                break;
            default:
                throw 1;
            }
            InterlockedExchange(&shared->result, 0);
        } catch (...) {
            InterlockedExchange(&shared->result, 1);
        }
        SetEvent(responseEvent);
    }
    engine.reset();
    UnmapViewOfFile(shared);
    return 0;
}
