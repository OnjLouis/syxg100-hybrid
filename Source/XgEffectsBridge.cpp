#include "XgEffectsBridge.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hybrid {
namespace {

constexpr std::array<std::uintptr_t, 2> voiceRenderCallRvas {
    0x18172,
    0x181c2,
};
constexpr std::uintptr_t expectedVoiceRenderRva = 0x18680;
constexpr std::uint8_t callOpcode = 0xe8;

using VoiceRender = void (__thiscall *)(void* state);

struct ActiveMix {
    void* context {};
    XgEffectsBridge::MixCallback callback {};
};

SRWLOCK bridgeLock = SRWLOCK_INIT;
HMODULE patchedModule {};
VoiceRender originalVoiceRender {};
LONG referenceCount {};
std::array<std::array<std::uint8_t, 5>, voiceRenderCallRvas.size()>
    originalCalls {};
thread_local ActiveMix activeMix;

void __fastcall voiceRenderHook(void* state, void*) noexcept
{
    originalVoiceRender(state);
    if (activeMix.callback == nullptr || state == nullptr)
        return;
    auto* buses = *reinterpret_cast<float**>(
        static_cast<std::byte*>(state) + sizeof(void*));
    if (buses != nullptr) {
        activeMix.callback(activeMix.context, buses,
                           XgEffectsBridge::quantumFrames);
    }
}

bool writeCall(std::uint8_t* call, const void* target) noexcept
{
    const auto next = reinterpret_cast<std::uintptr_t>(call + 5);
    const auto destination = reinterpret_cast<std::uintptr_t>(target);
    const auto displacement = static_cast<std::uint32_t>(destination - next);
    DWORD oldProtection {};
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;
    call[0] = callOpcode;
    std::memcpy(call + 1, &displacement, sizeof(displacement));
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    DWORD ignored {};
    VirtualProtect(call, 5, oldProtection, &ignored);
    return true;
}

} // namespace

bool XgEffectsBridge::acquire(HMODULE module) noexcept
{
    if (module == nullptr)
        return false;
    AcquireSRWLockExclusive(&bridgeLock);
    if (referenceCount != 0) {
        const auto compatible = patchedModule == module;
        if (compatible)
            ++referenceCount;
        ReleaseSRWLockExclusive(&bridgeLock);
        return compatible;
    }

    const auto expectedTarget = reinterpret_cast<std::uintptr_t>(module)
        + expectedVoiceRenderRva;
    for (const auto rva : voiceRenderCallRvas) {
        auto* call = reinterpret_cast<std::uint8_t*>(module) + rva;
        std::int32_t displacement {};
        std::memcpy(&displacement, call + 1, sizeof(displacement));
        const auto currentTarget = reinterpret_cast<std::uintptr_t>(call + 5)
            + displacement;
        if (call[0] != callOpcode || currentTarget != expectedTarget) {
            ReleaseSRWLockExclusive(&bridgeLock);
            return false;
        }
    }

    originalVoiceRender = reinterpret_cast<VoiceRender>(expectedTarget);
    std::size_t patchedCount = 0;
    for (std::size_t index = 0; index < voiceRenderCallRvas.size(); ++index) {
        auto* call = reinterpret_cast<std::uint8_t*>(module)
            + voiceRenderCallRvas[index];
        std::memcpy(originalCalls[index].data(), call,
                    originalCalls[index].size());
        if (!writeCall(call, reinterpret_cast<const void*>(&voiceRenderHook))) {
            for (std::size_t rollback = 0; rollback < patchedCount; ++rollback) {
                auto* previous = reinterpret_cast<std::uint8_t*>(module)
                    + voiceRenderCallRvas[rollback];
                DWORD oldProtection {};
                if (VirtualProtect(previous, originalCalls[rollback].size(),
                                   PAGE_EXECUTE_READWRITE, &oldProtection)) {
                    std::memcpy(previous, originalCalls[rollback].data(),
                                originalCalls[rollback].size());
                    DWORD ignored {};
                    VirtualProtect(previous, originalCalls[rollback].size(),
                                   oldProtection, &ignored);
                }
            }
            FlushInstructionCache(GetCurrentProcess(), module, 0);
            originalVoiceRender = nullptr;
            ReleaseSRWLockExclusive(&bridgeLock);
            return false;
        }
        ++patchedCount;
    }
    patchedModule = module;
    referenceCount = 1;
    ReleaseSRWLockExclusive(&bridgeLock);
    return true;
}

void XgEffectsBridge::release(HMODULE module) noexcept
{
    AcquireSRWLockExclusive(&bridgeLock);
    if (referenceCount == 0 || patchedModule != module) {
        ReleaseSRWLockExclusive(&bridgeLock);
        return;
    }
    if (--referenceCount == 0) {
        for (std::size_t index = 0; index < voiceRenderCallRvas.size(); ++index) {
            auto* call = reinterpret_cast<std::uint8_t*>(module)
                + voiceRenderCallRvas[index];
            DWORD oldProtection {};
            if (VirtualProtect(call, originalCalls[index].size(),
                               PAGE_EXECUTE_READWRITE, &oldProtection)) {
                std::memcpy(call, originalCalls[index].data(),
                            originalCalls[index].size());
                DWORD ignored {};
                VirtualProtect(call, originalCalls[index].size(), oldProtection,
                               &ignored);
            }
        }
        FlushInstructionCache(GetCurrentProcess(), module, 0);
        patchedModule = nullptr;
        originalVoiceRender = nullptr;
    }
    ReleaseSRWLockExclusive(&bridgeLock);
}

void XgEffectsBridge::beginBlock(void* context, MixCallback callback) noexcept
{
    activeMix = {context, callback};
}

void XgEffectsBridge::endBlock() noexcept
{
    activeMix = {};
}

} // namespace hybrid
