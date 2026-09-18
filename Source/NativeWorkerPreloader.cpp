#include "NativeWorkerPreloader.h"

#include <windows.h>

#include <exception>
#include <utility>

namespace hybrid {
namespace {

std::mutex preloadThrottle;

std::string currentFailure()
{
    try {
        throw;
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "unknown worker preload failure";
    }
}

} // namespace

NativeWorkerPreloader::NativeWorkerPreloader(
    std::filesystem::path vlWorkerPath,
    std::filesystem::path vlVxdPath,
    std::filesystem::path sgWorkerPath,
    std::filesystem::path sgVxdPath,
    std::uint32_t sampleRate, bool vlAvailable, bool sgAvailable)
    : vlWorkerPath(std::move(vlWorkerPath)),
      vlVxdPath(std::move(vlVxdPath)),
      sgWorkerPath(std::move(sgWorkerPath)),
      sgVxdPath(std::move(sgVxdPath)), sampleRate(sampleRate)
{
    vlStates.fill(vlAvailable ? State::queued : State::unavailable);
    sgState = sgAvailable ? State::queued : State::unavailable;
}

NativeWorkerPreloader::~NativeWorkerPreloader()
{
    stop();
}

void NativeWorkerPreloader::start() noexcept
{
    std::lock_guard lock(mutex);
    if (started || stopping)
        return;
    try {
        loader = std::thread([this] { run(); });
        started = true;
    } catch (...) {
        started = false;
    }
}

void NativeWorkerPreloader::stop()
{
    {
        std::lock_guard lock(mutex);
        if (stopping)
            return;
        stopping = true;
        changed.notify_all();
    }
    if (loader.joinable())
        loader.join();
    std::lock_guard lock(mutex);
    for (auto& client : vlClients)
        client.reset();
    sgClient.reset();
}

PreloadedWorker<NativeVlClient> NativeWorkerPreloader::takeVl(
    std::uint8_t voice)
{
    if (voice >= vlStates.size())
        return {};
    std::unique_lock lock(mutex);
    auto& state = vlStates[voice];
    if (state == State::queued || state == State::unavailable) {
        state = State::claimed;
        return {};
    }
    changed.wait(lock, [&] {
        return state != State::loading || stopping;
    });
    if (state == State::ready) {
        state = State::claimed;
        return { std::move(vlClients[voice]), {} };
    }
    if (state == State::failed) {
        state = State::claimed;
        return { nullptr, std::move(vlFailures[voice]) };
    }
    return {};
}

PreloadedWorker<NativeSgClient> NativeWorkerPreloader::takeSg()
{
    std::unique_lock lock(mutex);
    if (sgState == State::queued || sgState == State::unavailable) {
        sgState = State::claimed;
        return {};
    }
    changed.wait(lock, [&] {
        return sgState != State::loading || stopping;
    });
    if (sgState == State::ready) {
        sgState = State::claimed;
        return { std::move(sgClient), {} };
    }
    if (sgState == State::failed) {
        sgState = State::claimed;
        return { nullptr, std::move(sgFailure) };
    }
    return {};
}

void NativeWorkerPreloader::run()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    loadVl(0);
    loadSg();
    for (std::uint8_t voice = 1; voice < vlVoiceCount; ++voice)
        loadVl(voice);
}

void NativeWorkerPreloader::loadVl(std::uint8_t voice)
{
    {
        std::lock_guard lock(mutex);
        if (stopping || vlStates[voice] != State::queued)
            return;
        vlStates[voice] = State::loading;
    }
    std::unique_ptr<NativeVlClient> client;
    std::string failure;
    try {
        std::lock_guard launchLock(preloadThrottle);
        client = std::make_unique<NativeVlClient>(
            vlWorkerPath, vlVxdPath, sampleRate);
    } catch (...) {
        failure = currentFailure();
    }
    std::lock_guard lock(mutex);
    if (stopping) {
        vlStates[voice] = State::claimed;
    } else if (client != nullptr) {
        vlClients[voice] = std::move(client);
        vlStates[voice] = State::ready;
    } else {
        vlFailures[voice] = std::move(failure);
        vlStates[voice] = State::failed;
    }
    changed.notify_all();
}

void NativeWorkerPreloader::loadSg()
{
    {
        std::lock_guard lock(mutex);
        if (stopping || sgState != State::queued)
            return;
        sgState = State::loading;
    }
    std::unique_ptr<NativeSgClient> client;
    std::string failure;
    try {
        std::lock_guard launchLock(preloadThrottle);
        client = std::make_unique<NativeSgClient>(
            sgWorkerPath, sgVxdPath, sampleRate);
    } catch (...) {
        failure = currentFailure();
    }
    std::lock_guard lock(mutex);
    if (stopping) {
        sgState = State::claimed;
    } else if (client != nullptr) {
        sgClient = std::move(client);
        sgState = State::ready;
    } else {
        sgFailure = std::move(failure);
        sgState = State::failed;
    }
    changed.notify_all();
}

} // namespace hybrid
