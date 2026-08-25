#include "NativeVlClient.h"

#include "NativeVlProtocol.h"

#include <windows.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace hybrid {
namespace {

std::wstring quoted(const std::filesystem::path& path)
{
    return L"\"" + path.wstring() + L"\"";
}

std::wstring handleText(HANDLE handle)
{
    return std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
}

} // namespace

class NativeVlClient::Impl {
public:
    Impl(const std::filesystem::path& workerPath,
         const std::filesystem::path& vxdPath, std::uint32_t sampleRate)
    {
        try {
        SECURITY_ATTRIBUTES security { sizeof(security), nullptr, TRUE };
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security,
                                     PAGE_READWRITE, 0,
                                     static_cast<DWORD>(sizeof(ipc::SharedState)),
                                     nullptr);
        requestEvent = CreateEventW(&security, FALSE, FALSE, nullptr);
        responseEvent = CreateEventW(&security, FALSE, FALSE, nullptr);
        if (mapping == nullptr || requestEvent == nullptr
            || responseEvent == nullptr) {
            throw std::runtime_error("cannot create VL worker IPC objects");
        }
        shared = static_cast<ipc::SharedState*>(MapViewOfFile(
            mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::SharedState)));
        if (shared == nullptr)
            throw std::runtime_error("cannot map VL worker IPC state");
        std::memset(shared, 0, sizeof(*shared));

        std::wstring command = quoted(workerPath) + L" " + handleText(mapping)
            + L" " + handleText(requestEvent) + L" "
            + handleText(responseEvent) + L" " + std::to_wstring(sampleRate)
            + L" " + quoted(vxdPath);
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        STARTUPINFOEXW startup {};
        startup.StartupInfo.cb = sizeof(startup);
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        startup.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, attributeBytes));
        if (startup.lpAttributeList == nullptr
            || !InitializeProcThreadAttributeList(startup.lpAttributeList, 1,
                                                  0, &attributeBytes)) {
            if (startup.lpAttributeList != nullptr)
                HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
            throw std::runtime_error("cannot initialize VL worker handle list");
        }
        std::array<HANDLE, 3> inherited { mapping, requestEvent, responseEvent };
        if (!UpdateProcThreadAttribute(
                startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited.data(), sizeof(inherited), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
            throw std::runtime_error("cannot configure VL worker handle list");
        }

        PROCESS_INFORMATION processInfo {};
        const auto created = CreateProcessW(
            workerPath.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
            workerPath.parent_path().c_str(), &startup.StartupInfo,
            &processInfo);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
        if (!created)
            throw std::runtime_error("cannot launch native VL worker");
        process = processInfo.hProcess;
        CloseHandle(processInfo.hThread);
        waitForResponse("initialization", 10'000);
        } catch (...) {
            cleanup(true);
            throw;
        }
    }

    ~Impl() { cleanup(false); }

    void cleanup(bool force) noexcept
    {
        if (process != nullptr
            && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            if (force || unresponsive) {
                TerminateProcess(process, 1);
            } else {
                try {
                    request(ipc::Command::shutdown);
                } catch (...) {
                    TerminateProcess(process, 1);
                }
            }
            WaitForSingleObject(process, 2'000);
        }
        if (process != nullptr)
            CloseHandle(process);
        if (shared != nullptr)
            UnmapViewOfFile(shared);
        if (mapping != nullptr)
            CloseHandle(mapping);
        if (requestEvent != nullptr)
            CloseHandle(requestEvent);
        if (responseEvent != nullptr)
            CloseHandle(responseEvent);
        process = nullptr;
        shared = nullptr;
        mapping = nullptr;
        requestEvent = nullptr;
        responseEvent = nullptr;
    }

    void request(ipc::Command command, std::uint32_t argument = 0)
    {
        shared->argument = argument;
        InterlockedExchange(&shared->result, 0);
        InterlockedExchange(&shared->command, static_cast<LONG>(command));
        if (!SetEvent(requestEvent))
            throw std::runtime_error("cannot signal native VL worker");
        try {
            waitForResponse("request", 2'000);
        } catch (...) {
            unresponsive = true;
            throw;
        }
    }

    void waitForResponse(const char* stage, DWORD timeout)
    {
        const std::array<HANDLE, 2> handles { responseEvent, process };
        const auto result = WaitForMultipleObjects(
            static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout);
        if (result != WAIT_OBJECT_0 || shared->result != 0)
            throw std::runtime_error(std::string("native VL worker ") + stage
                                     + " failed");
    }

    HANDLE mapping {};
    HANDLE requestEvent {};
    HANDLE responseEvent {};
    HANDLE process {};
    ipc::SharedState* shared {};
    bool unresponsive {};
};

NativeVlClient::NativeVlClient(const std::filesystem::path& workerPath,
                               const std::filesystem::path& vxdPath,
                               std::uint32_t sampleRate)
    : impl(std::make_unique<Impl>(workerPath, vxdPath, sampleRate))
{
}

NativeVlClient::~NativeVlClient() = default;

void NativeVlClient::sendShort(std::uint32_t packedMessage)
{
    impl->request(ipc::Command::sendShort, packedMessage);
}

void NativeVlClient::sendSysex(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > impl->shared->data.size())
        throw std::runtime_error("VL SysEx exceeds IPC capacity");
    std::memcpy(impl->shared->data.data(), bytes.data(), bytes.size());
    impl->shared->dataSize = static_cast<std::uint32_t>(bytes.size());
    impl->request(ipc::Command::sendSysex);
}

void NativeVlClient::setSampleRate(std::uint32_t sampleRate)
{
    impl->request(ipc::Command::setSampleRate, sampleRate);
}

void NativeVlClient::prepare()
{
    impl->request(ipc::Command::prepare);
}

void NativeVlClient::render(std::uint32_t frames)
{
    impl->request(ipc::Command::render, frames);
}

std::span<const std::int16_t> NativeVlClient::plane(
    std::size_t index, std::uint32_t frames) const
{
    if (index >= NativeVlClient::planeCount
        || frames > NativeVlClient::maxFrames) {
        return {};
    }
    return {
        impl->shared->audio.data() + index * ipc::maxStereoSamples,
        static_cast<std::size_t>(frames) * 2,
    };
}

} // namespace hybrid
