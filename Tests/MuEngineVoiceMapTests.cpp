#include "MuEngineVoiceMap.h"
#include <cassert>
#include <cstring>

namespace {
std::array<std::uint8_t, 36> state {};
int stateSize = 36;
int writes = 0;
vst2::IntPtr dispatch(vst2::AEffect*, std::int32_t opcode, std::int32_t,
                      vst2::IntPtr size, void* data, float)
{
    if (opcode == 23) {
        *static_cast<void**>(data) = state.data();
        return stateSize;
    }
    if (opcode == 24) {
        assert(size == stateSize);
        std::memcpy(state.data(), data, size);
        ++writes;
        return 1;
    }
    assert(false);
    return 0;
}
}

int main()
{
    vst2::AEffect child {};
    child.dispatcher = dispatch;
    state[33] = 'V'; state[34] = 'M';
    const std::array<std::uint8_t, 9> basic {0xf0,0x43,0x10,0x49,0,0,0x12,0,0xf7};
    auto native = basic; native[7] = 1;
    assert(hybrid::applyMuEngineVoiceMap(&child, basic));
    assert(state[35] == 1 && writes == 1);
    assert(hybrid::applyMuEngineVoiceMap(&child, native));
    assert(state[35] == 0 && writes == 2);
    auto invalid = basic; invalid[7] = 2;
    assert(!hybrid::applyMuEngineVoiceMap(&child, invalid));
    invalid = basic; invalid[3] = 0x4c;
    assert(!hybrid::applyMuEngineVoiceMap(&child, invalid));
    assert(!hybrid::applyMuEngineVoiceMap(nullptr, basic));
    stateSize = 33;
    assert(!hybrid::applyMuEngineVoiceMap(&child, basic));
    stateSize = 36; state[33] = 'X';
    assert(!hybrid::applyMuEngineVoiceMap(&child, basic));
    assert(writes == 2);
}
