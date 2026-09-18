#pragma once

#include "NativeSgClient.h"
#include "NativeVlClient.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace hybrid {

template <typename Client>
struct PreloadedWorker {
    std::unique_ptr<Client> client;
    std::string failure;
};

class NativeWorkerPreloader {
public:
    static constexpr std::size_t vlVoiceCount = 8;

    NativeWorkerPreloader(std::filesystem::path vlWorkerPath,
                          std::filesystem::path vlVxdPath,
                          std::filesystem::path sgWorkerPath,
                          std::filesystem::path sgVxdPath,
                          std::uint32_t sampleRate, bool vlAvailable,
                          bool sgAvailable);
    ~NativeWorkerPreloader();

    NativeWorkerPreloader(const NativeWorkerPreloader&) = delete;
    NativeWorkerPreloader& operator=(const NativeWorkerPreloader&) = delete;

    void start() noexcept;
    void stop();
    [[nodiscard]] PreloadedWorker<NativeVlClient> takeVl(std::uint8_t voice);
    [[nodiscard]] PreloadedWorker<NativeSgClient> takeSg();

private:
    enum class State : std::uint8_t {
        unavailable,
        queued,
        loading,
        ready,
        claimed,
        failed,
    };

    void run();
    void loadVl(std::uint8_t voice);
    void loadSg();

    std::filesystem::path vlWorkerPath;
    std::filesystem::path vlVxdPath;
    std::filesystem::path sgWorkerPath;
    std::filesystem::path sgVxdPath;
    std::uint32_t sampleRate {};
    std::array<State, vlVoiceCount> vlStates;
    std::array<std::unique_ptr<NativeVlClient>, vlVoiceCount> vlClients;
    std::array<std::string, vlVoiceCount> vlFailures;
    State sgState { State::unavailable };
    std::unique_ptr<NativeSgClient> sgClient;
    std::string sgFailure;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread loader;
    bool started {};
    bool stopping {};
};

} // namespace hybrid
