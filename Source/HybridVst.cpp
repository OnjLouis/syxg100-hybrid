#include "MidiRouter.h"
#include "NativeVlClient.h"
#include "Vst2Abi.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <span>

namespace {

constexpr std::size_t maxPendingVlEvents = 512;
constexpr std::size_t childEventsPerBatch = 64;
constexpr std::size_t maxChildEventBatches = 8;
constexpr float int16Scale = 1.0f / 32768.0f;

struct PendingMidi {
    std::int32_t deltaFrames {};
    std::uint32_t message {};
};

struct ChildEventBatch {
    std::int32_t numEvents {};
    vst2::IntPtr reserved {};
    std::array<vst2::Event*, childEventsPerBatch> events {};
};

struct WrapperState {
    HMODULE module {};
    vst2::AEffect* child {};
    std::filesystem::path vxdPath;
    std::filesystem::path workerPath;
    std::unique_ptr<hybrid::NativeVlClient> vl;
    hybrid::MidiRouter router;
    std::array<PendingMidi, maxPendingVlEvents> pendingVl {};
    std::size_t pendingVlCount {};
    std::array<ChildEventBatch, maxChildEventBatches> childBatches {};
    std::size_t childBatchCount {};
    float sampleRate {};
    bool vlPrepared {};
    bool vlHasRendered {};
};

WrapperState* state(vst2::AEffect* effect)
{
    return static_cast<WrapperState*>(effect->object);
}

void reportVlFailure(const char* context)
{
    OutputDebugStringA("S-YXG100 Hybrid: VL engine disabled after ");
    OutputDebugStringA(context);
    OutputDebugStringA("\n");
}

void disableVl(WrapperState& wrapper, const char* context)
{
    reportVlFailure(context);
    wrapper.pendingVlCount = 0;
    wrapper.vlPrepared = false;
    wrapper.vlHasRendered = false;
    wrapper.vl.reset();
}

void configureVl(WrapperState& wrapper, float sampleRate)
{
    if (sampleRate <= 0.0f || !std::filesystem::is_regular_file(wrapper.vxdPath))
        return;
    try {
        if (wrapper.vl == nullptr) {
            wrapper.vl = std::make_unique<hybrid::NativeVlClient>(
                wrapper.workerPath, wrapper.vxdPath,
                static_cast<std::uint32_t>(std::lround(sampleRate)));
            wrapper.vlHasRendered = false;
        } else if (wrapper.sampleRate != sampleRate) {
            wrapper.vl->setSampleRate(
                static_cast<std::uint32_t>(std::lround(sampleRate)));
        }
        wrapper.sampleRate = sampleRate;
    } catch (...) {
        disableVl(wrapper, "initialization failure");
    }
}

std::uint32_t packedMessage(const vst2::MidiEvent& event)
{
    std::uint32_t packed = 0;
    std::memcpy(&packed, event.midiData, sizeof(event.midiData));
    return packed;
}

bool isNoteMessage(std::uint32_t packed)
{
    const auto operation = static_cast<std::uint8_t>(packed & 0xf0);
    return operation == 0x80 || operation == 0x90;
}

bool isSystemReset(std::span<const std::uint8_t> bytes)
{
    const bool gmReset = bytes.size() >= 6 && bytes[0] == 0xf0
        && bytes[1] == 0x7e && bytes[3] == 0x09 && bytes[4] == 0x01;
    const bool xgReset = bytes.size() >= 9 && bytes[0] == 0xf0
        && bytes[1] == 0x43 && bytes[3] == 0x4c && bytes[4] == 0x00
        && bytes[5] == 0x00 && bytes[6] == 0x7e && bytes[7] == 0x00;
    return gmReset || xgReset;
}

void queueVl(WrapperState& wrapper, std::int32_t deltaFrames,
             std::uint32_t message)
{
    if (wrapper.pendingVlCount < wrapper.pendingVl.size()) {
        wrapper.pendingVl[wrapper.pendingVlCount++] = { deltaFrames, message };
        return;
    }
    wrapper.vl->sendShort(message);
}

void retainChildEvent(WrapperState& wrapper, vst2::Event* event, bool forceNew)
{
    if (forceNew || wrapper.childBatchCount == 0
        || wrapper.childBatches[wrapper.childBatchCount - 1].numEvents
            == childEventsPerBatch) {
        if (wrapper.childBatchCount == wrapper.childBatches.size())
            return;
        ++wrapper.childBatchCount;
    }
    auto& batch = wrapper.childBatches[wrapper.childBatchCount - 1];
    batch.events[batch.numEvents++] = event;
}

void clearChildEvents(WrapperState& wrapper)
{
    for (std::size_t index = 0; index < wrapper.childBatchCount; ++index)
        wrapper.childBatches[index].numEvents = 0;
    wrapper.childBatchCount = 0;
}

vst2::IntPtr processEvents(WrapperState& wrapper, const vst2::Events* events)
{
    if (events == nullptr)
        return 0;
    if (wrapper.vl == nullptr) {
        return wrapper.child->dispatcher(wrapper.child, vst2::processEvents, 0,
                                         0, const_cast<vst2::Events*>(events),
                                         0.0f);
    }
    const auto firstNewBatch = wrapper.childBatchCount;
    bool firstChildEvent = true;
    vst2::IntPtr result = 0;
    try {
        for (std::int32_t index = 0; index < events->numEvents; ++index) {
            auto* event = events->events[index];
            bool sendToChild = true;
            if (event != nullptr && event->type == 1) {
                const auto* midi = reinterpret_cast<const vst2::MidiEvent*>(event);
                const auto packed = packedMessage(*midi);
                const auto destination = wrapper.router.routeShortMessage(packed);
                sendToChild = destination != hybrid::MidiDestination::vl;
                if (destination != hybrid::MidiDestination::xg) {
                    if (!wrapper.vlPrepared && isNoteMessage(packed)) {
                        wrapper.vl->prepare();
                        wrapper.vlPrepared = true;
                    }
                    if (wrapper.vlPrepared)
                        queueVl(wrapper, midi->deltaFrames, packed);
                    else
                        wrapper.vl->sendShort(packed);
                }
            } else if (event != nullptr && event->type == 6) {
                const auto* sysex = reinterpret_cast<const vst2::SysexEvent*>(event);
                if (sysex->sysexDump != nullptr && sysex->dumpBytes > 0) {
                    const std::span<const std::uint8_t> bytes {
                        reinterpret_cast<const std::uint8_t*>(sysex->sysexDump),
                        static_cast<std::size_t>(sysex->dumpBytes),
                    };
                    wrapper.vl->sendSysex(bytes);
                    if (isSystemReset(bytes))
                        wrapper.router.reset();
                }
            }

            if (sendToChild && event != nullptr) {
                retainChildEvent(wrapper, event, firstChildEvent);
                firstChildEvent = false;
            }
        }
    } catch (...) {
        disableVl(wrapper, "MIDI processing failure");
    }
    for (std::size_t index = firstNewBatch;
         index < wrapper.childBatchCount; ++index) {
        result |= wrapper.child->dispatcher(
            wrapper.child, vst2::processEvents, 0, 0,
            &wrapper.childBatches[index], 0.0f);
    }
    return result;
}

void renderVlSegment(WrapperState& wrapper, float** outputs,
                     std::int32_t outputOffset, std::int32_t frames)
{
    while (frames > 0) {
        const auto block = static_cast<std::uint32_t>(std::min<std::int32_t>(
            frames, hybrid::NativeVlClient::maxFrames));
        wrapper.vl->render(block);
        wrapper.vlHasRendered = true;
        if (outputs != nullptr && outputs[0] != nullptr && outputs[1] != nullptr) {
            for (std::size_t plane = 0;
                 plane < hybrid::NativeVlClient::planeCount; ++plane) {
                const auto stereo = wrapper.vl->plane(plane, block);
                for (std::uint32_t frame = 0; frame < block; ++frame) {
                    outputs[0][outputOffset + frame] += stereo[frame * 2]
                        * int16Scale;
                    outputs[1][outputOffset + frame] += stereo[frame * 2 + 1]
                        * int16Scale;
                }
            }
        }
        outputOffset += static_cast<std::int32_t>(block);
        frames -= static_cast<std::int32_t>(block);
    }
}

void mixVl(WrapperState& wrapper, float** outputs, std::int32_t frames)
{
    if (wrapper.vl == nullptr || !wrapper.vlPrepared || frames <= 0) {
        wrapper.pendingVlCount = 0;
        return;
    }
    try {
        std::int32_t position = 0;
        for (std::size_t index = 0; index < wrapper.pendingVlCount; ++index) {
            const auto eventPosition = std::clamp(
                wrapper.pendingVl[index].deltaFrames, position, frames);
            if (wrapper.vlHasRendered) {
                renderVlSegment(wrapper, outputs, position,
                                eventPosition - position);
            }
            wrapper.vl->sendShort(wrapper.pendingVl[index].message);
            position = eventPosition;
        }
        renderVlSegment(wrapper, outputs, position, frames - position);
        wrapper.pendingVlCount = 0;
    } catch (...) {
        disableVl(wrapper, "audio rendering failure");
    }
}

vst2::IntPtr dispatch(vst2::AEffect* effect, std::int32_t opcode,
                      std::int32_t index, vst2::IntPtr value, void* data,
                      float option)
{
    auto* wrapper = state(effect);
    if (wrapper == nullptr || wrapper->child == nullptr)
        return 0;
    if (opcode == vst2::processEvents)
        return processEvents(*wrapper, static_cast<const vst2::Events*>(data));
    if (opcode != vst2::close) {
        const auto result = wrapper->child->dispatcher(
            wrapper->child, opcode, index, value, data, option);
        if (opcode == vst2::setSampleRate)
            configureVl(*wrapper, option);
        return result;
    }

    const auto result = wrapper->child->dispatcher(wrapper->child, opcode, index,
                                                   value, data, option);
    wrapper->vl.reset();
    if (wrapper->module != nullptr)
        FreeLibrary(wrapper->module);
    delete wrapper;
    delete effect;
    return result;
}

void process(vst2::AEffect* effect, float** inputs, float** outputs,
             std::int32_t frames)
{
    auto& wrapper = *state(effect);
    if (wrapper.child->process != nullptr)
        wrapper.child->process(wrapper.child, inputs, outputs, frames);
    clearChildEvents(wrapper);
    mixVl(wrapper, outputs, frames);
}

void processReplacing(vst2::AEffect* effect, float** inputs, float** outputs,
                      std::int32_t frames)
{
    auto& wrapper = *state(effect);
    if (wrapper.child->processReplacing != nullptr)
        wrapper.child->processReplacing(wrapper.child, inputs, outputs, frames);
    else if (wrapper.child->process != nullptr)
        wrapper.child->process(wrapper.child, inputs, outputs, frames);
    clearChildEvents(wrapper);
    mixVl(wrapper, outputs, frames);
}

void setParameter(vst2::AEffect* effect, std::int32_t index, float value)
{
    auto* child = state(effect)->child;
    child->setParameter(child, index, value);
}

float getParameter(vst2::AEffect* effect, std::int32_t index)
{
    auto* child = state(effect)->child;
    return child->getParameter(child, index);
}

} // namespace

extern "C" __declspec(dllexport) vst2::AEffect* VSTPluginMain(
    vst2::HostCallback host)
{
    wchar_t wrapperPath[MAX_PATH] {};
    HMODULE self {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&VSTPluginMain), &self)
        || GetModuleFileNameW(self, wrapperPath, MAX_PATH) == 0) {
        return nullptr;
    }

    const std::filesystem::path directory =
        std::filesystem::path(wrapperPath).parent_path();
    const auto childPath = directory / L"syxg50-engine.dll";
    const auto module = LoadLibraryW(childPath.c_str());
    if (module == nullptr)
        return nullptr;
    const auto entry = reinterpret_cast<vst2::EntryPoint>(
        GetProcAddress(module, "main"));
    if (entry == nullptr) {
        FreeLibrary(module);
        return nullptr;
    }

    const auto vxdPath = directory / L"Sxgpvknl.vxd";
    const auto reportedRate = static_cast<float>(
        host(nullptr, vst2::hostGetSampleRate, 0, 0, nullptr, 0.0f));
    const auto initialRate = reportedRate > 0.0f ? reportedRate : 44'100.0f;
    const auto workerPath = directory / L"syxg100-vl-worker.exe";
    std::unique_ptr<hybrid::NativeVlClient> initialVl;
    if (std::filesystem::is_regular_file(vxdPath)
        && std::filesystem::is_regular_file(workerPath)) {
        try {
            initialVl = std::make_unique<hybrid::NativeVlClient>(
                workerPath, vxdPath,
                static_cast<std::uint32_t>(std::lround(initialRate)));
        } catch (...) {
            reportVlFailure("initialization failure");
        }
    }

    auto* child = entry(host);
    if (child == nullptr || child->magic != vst2::effectMagic) {
        FreeLibrary(module);
        return nullptr;
    }

    auto* wrapperState = new (std::nothrow) WrapperState;
    auto* effect = new (std::nothrow) vst2::AEffect {};
    if (wrapperState == nullptr || effect == nullptr) {
        delete wrapperState;
        delete effect;
        child->dispatcher(child, vst2::close, 0, 0, nullptr, 0.0f);
        FreeLibrary(module);
        return nullptr;
    }
    wrapperState->module = module;
    wrapperState->child = child;
    wrapperState->vxdPath = vxdPath;
    wrapperState->workerPath = workerPath;
    wrapperState->vl = std::move(initialVl);
    wrapperState->sampleRate = initialRate;

    *effect = *child;
    effect->dispatcher = dispatch;
    effect->process = process;
    effect->setParameter = setParameter;
    effect->getParameter = getParameter;
    effect->object = wrapperState;
    effect->processReplacing = processReplacing;
    return effect;
}
