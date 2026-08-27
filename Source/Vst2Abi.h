#pragma once

#include <cstdint>

namespace vst2 {

using IntPtr = std::intptr_t;

struct AEffect;
using HostCallback = IntPtr (*)(AEffect*, std::int32_t, std::int32_t, IntPtr,
                                void*, float);
using Dispatcher = IntPtr (*)(AEffect*, std::int32_t, std::int32_t, IntPtr,
                              void*, float);
using Process = void (*)(AEffect*, float**, float**, std::int32_t);
using SetParameter = void (*)(AEffect*, std::int32_t, float);
using GetParameter = float (*)(AEffect*, std::int32_t);

struct AEffect {
    std::int32_t magic;
    Dispatcher dispatcher;
    Process process;
    SetParameter setParameter;
    GetParameter getParameter;
    std::int32_t numPrograms;
    std::int32_t numParams;
    std::int32_t numInputs;
    std::int32_t numOutputs;
    std::int32_t flags;
    void* reserved1;
    void* reserved2;
    std::int32_t initialDelay;
    std::int32_t realQualities;
    std::int32_t offQualities;
    float ioRatio;
    void* object;
    void* user;
    std::int32_t uniqueId;
    std::int32_t version;
    Process processReplacing;
    Process processDoubleReplacing;
    char future[56];
};

struct Event {
    std::int32_t type;
    std::int32_t byteSize;
    std::int32_t deltaFrames;
    std::int32_t flags;
    char data[16];
};

struct MidiEvent {
    std::int32_t type { 1 };
    std::int32_t byteSize { sizeof(MidiEvent) };
    std::int32_t deltaFrames {};
    std::int32_t flags {};
    std::int32_t noteLength {};
    std::int32_t noteOffset {};
    char midiData[4] {};
    char detune {};
    char noteOffVelocity {};
    char reserved1 {};
    char reserved2 {};
};

struct SysexEvent {
    std::int32_t type { 6 };
    std::int32_t byteSize { sizeof(SysexEvent) };
    std::int32_t deltaFrames {};
    std::int32_t flags {};
    std::int32_t dumpBytes {};
    IntPtr reserved1 {};
    char* sysexDump {};
    IntPtr reserved2 {};
};

struct Events {
    std::int32_t numEvents;
    IntPtr reserved {};
    Event* events[2] {};
};

using EntryPoint = AEffect* (*)(HostCallback);

constexpr std::int32_t effectMagic = 0x56737450;

enum EffectOpcode : std::int32_t {
    open = 0,
    close = 1,
    setSampleRate = 10,
    setBlockSize = 11,
    mainsChanged = 12,
    processEvents = 25,
    getEffectName = 45,
    getVendorString = 47,
    getProductString = 48,
    getVendorVersion = 49,
};

enum HostOpcode : std::int32_t {
    hostVersion = 1,
    hostGetSampleRate = 16,
    hostGetBlockSize = 17,
};

} // namespace vst2
