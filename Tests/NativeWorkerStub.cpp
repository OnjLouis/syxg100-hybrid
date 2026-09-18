#include "../Source/NativeVlProtocol.h"

#include <windows.h>

#include <cstdint>

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
    auto* shared = static_cast<hybrid::ipc::SharedState*>(MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0,
        sizeof(hybrid::ipc::SharedState)));
    if (shared == nullptr)
        return 65;

    InterlockedExchange(&shared->result, 0);
    SetEvent(responseEvent);
    bool running = true;
    while (running
           && WaitForSingleObject(requestEvent, INFINITE) == WAIT_OBJECT_0) {
        const auto command = static_cast<hybrid::ipc::Command>(
            InterlockedExchange(&shared->command, 0));
        if (command == hybrid::ipc::Command::shutdown)
            running = false;
        else if (command == hybrid::ipc::Command::getRouteMask)
            shared->argument = 0;
        InterlockedExchange(&shared->result, 0);
        SetEvent(responseEvent);
    }
    UnmapViewOfFile(shared);
    return 0;
}
