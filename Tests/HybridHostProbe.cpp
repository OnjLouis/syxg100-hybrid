#include "../Source/Vst2Abi.h"

#include <windows.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr std::int32_t blockSize = 512;
constexpr float sampleRate = 44'100.0f;

vst2::IntPtr host(vst2::AEffect*, std::int32_t opcode, std::int32_t,
                  vst2::IntPtr, void*, float)
{
    switch (opcode) {
    case vst2::hostVersion: return 2400;
    case vst2::hostGetSampleRate: return static_cast<vst2::IntPtr>(sampleRate);
    case vst2::hostGetBlockSize: return blockSize;
    default: return 0;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: HybridHostProbe <wrapper.dll>\n");
        return 2;
    }
    const auto module = LoadLibraryA(argv[1]);
    if (module == nullptr) {
        std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 3;
    }
    const auto entry = reinterpret_cast<vst2::EntryPoint>(
        GetProcAddress(module, "main"));
    if (entry == nullptr) {
        std::fprintf(stderr, "missing VST main export\n");
        FreeLibrary(module);
        return 4;
    }
    auto* effect = entry(host);
    if (effect == nullptr || effect->magic != vst2::effectMagic) {
        std::fprintf(stderr, "wrapper did not return a valid AEffect\n");
        FreeLibrary(module);
        return 5;
    }

    std::array<char, 64> name {};
    std::array<char, 64> vendor {};
    effect->dispatcher(effect, vst2::open, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, vst2::getEffectName, 0, 0, name.data(), 0.0f);
    effect->dispatcher(effect, vst2::getVendorString, 0, 0, vendor.data(), 0.0f);
    effect->dispatcher(effect, vst2::setSampleRate, 0, 0, nullptr, sampleRate);
    effect->dispatcher(effect, vst2::setBlockSize, 0, blockSize, nullptr, 0.0f);
    effect->dispatcher(effect, vst2::mainsChanged, 0, 1, nullptr, 0.0f);

    std::vector<float> left(blockSize);
    std::vector<float> right(blockSize);
    std::array<float*, 2> outputs { left.data(), right.data() };
    effect->processReplacing(effect, nullptr, outputs.data(), blockSize);

    vst2::MidiEvent noteOn;
    noteOn.midiData[0] = static_cast<char>(0x90);
    noteOn.midiData[1] = 60;
    noteOn.midiData[2] = 100;
    vst2::Events events { 1 };
    events.events[0] = reinterpret_cast<vst2::Event*>(&noteOn);
    effect->dispatcher(effect, vst2::processEvents, 0, 0, &events, 0.0f);

    float peak = 0.0f;
    for (int block = 0; block < 100; ++block) {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        effect->processReplacing(effect, nullptr, outputs.data(), blockSize);
        for (const auto sample : left)
            peak = std::max(peak, std::abs(sample));
        for (const auto sample : right)
            peak = std::max(peak, std::abs(sample));
    }

    std::printf("name=%s vendor=%s programs=%d parameters=%d outputs=%d peak=%g\n",
                name.data(), vendor.data(), effect->numPrograms,
                effect->numParams, effect->numOutputs, peak);
    effect->dispatcher(effect, vst2::mainsChanged, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, vst2::close, 0, 0, nullptr, 0.0f);
    FreeLibrary(module);
    return peak > 0.0f && std::isfinite(peak) ? 0 : 6;
}
