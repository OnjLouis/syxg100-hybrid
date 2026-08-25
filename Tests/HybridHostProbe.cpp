#include "../Source/Vst2Abi.h"

#include <windows.h>

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::int32_t blockSize = 512;
constexpr float sampleRate = 44'100.0f;

LONG WINAPI reportCrash(EXCEPTION_POINTERS* details)
{
    const auto* record = details->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION
        || record->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION
        || record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        std::fprintf(stderr,
                     "exception=0x%08lx address=%p eip=0x%08lx esp=0x%08lx\n",
                     record->ExceptionCode, record->ExceptionAddress,
                     details->ContextRecord->Eip, details->ContextRecord->Esp);
        std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

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
    AddVectoredExceptionHandler(0, reportCrash);
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr,
                     "usage: HybridHostProbe <wrapper.dll> [events.pvte.txt]\n");
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

    float peak = 0.0f;
    std::uint64_t sampleHash = 1469598103934665603ull;
    bool timingPassed = true;
    bool idleTimingChecked = false;
    const auto renderBlock = [&] {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        effect->processReplacing(effect, nullptr, outputs.data(), blockSize);
        for (const auto sample : left)
            peak = std::max(peak, std::abs(sample));
        for (const auto sample : right)
            peak = std::max(peak, std::abs(sample));
        for (const auto* channel : outputs) {
            for (std::int32_t frame = 0; frame < blockSize; ++frame) {
                sampleHash ^= std::bit_cast<std::uint32_t>(channel[frame]);
                sampleHash *= 1099511628211ull;
            }
        }
    };
    const auto sendEvent = [&](vst2::Event* event) {
        vst2::Events events { 1 };
        events.events[0] = event;
        effect->dispatcher(effect, vst2::processEvents, 0, 0, &events, 0.0f);
        renderBlock();
    };

    if (argc == 2) {
        vst2::MidiEvent noteOn;
        noteOn.midiData[0] = static_cast<char>(0x90);
        noteOn.midiData[1] = 60;
        noteOn.midiData[2] = 100;
        sendEvent(reinterpret_cast<vst2::Event*>(&noteOn));
    } else {
        std::ifstream input(argv[2]);
        std::string line;
        if (!input || !std::getline(input, line) || line != "PVTE 1")
            throw std::runtime_error("invalid event trace");
        while (std::getline(input, line)) {
            if (line.size() < 3 || line[1] != ' ')
                continue;
            const auto hex = std::string_view(line).substr(2);
            if ((hex.size() % 2) != 0)
                throw std::runtime_error("odd event trace record");
            std::vector<std::uint8_t> bytes(hex.size() / 2);
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                bytes[index] = static_cast<std::uint8_t>(std::stoul(
                    std::string(hex.substr(index * 2, 2)), nullptr, 16));
            }
            if (line[0] == 'M') {
                vst2::MidiEvent midi;
                std::copy_n(bytes.begin(), std::min<std::size_t>(bytes.size(), 4),
                            midi.midiData);
                const auto deltaText = std::getenv("HYBRID_PROBE_NOTE_DELTA");
                const auto operation = static_cast<std::uint8_t>(bytes[0] & 0xf0);
                if (deltaText != nullptr
                    && (operation == 0x80 || operation == 0x90)) {
                    midi.deltaFrames = std::clamp(std::atoi(deltaText), 0,
                                                  blockSize);
                }
                sendEvent(reinterpret_cast<vst2::Event*>(&midi));
                if (midi.deltaFrames > 0) {
                    float prefixPeak = 0.0f;
                    float suffixPeak = 0.0f;
                    for (std::int32_t frame = 0; frame < blockSize; ++frame) {
                        auto& segment = frame < midi.deltaFrames
                            ? prefixPeak : suffixPeak;
                        segment = std::max(segment, std::abs(left[frame]));
                        segment = std::max(segment, std::abs(right[frame]));
                    }
                    std::printf("timing prefix=%g suffix=%g delta=%d\n",
                                prefixPeak, suffixPeak, midi.deltaFrames);
                    if (!idleTimingChecked) {
                        timingPassed &= prefixPeak == 0.0f;
                        idleTimingChecked = true;
                    }
                }
            } else if (line[0] == 'S') {
                vst2::SysexEvent sysex;
                sysex.dumpBytes = static_cast<std::int32_t>(bytes.size());
                sysex.sysexDump = reinterpret_cast<char*>(bytes.data());
                sendEvent(reinterpret_cast<vst2::Event*>(&sysex));
            }
        }
    }

    const auto blockText = std::getenv("HYBRID_PROBE_BLOCKS");
    const auto renderBlocks = blockText == nullptr ? 100
        : std::max(1, std::atoi(blockText));
    for (int block = 0; block < renderBlocks; ++block) {
        renderBlock();
    }

    std::printf("name=%s vendor=%s programs=%d parameters=%d outputs=%d "
                "peak=%g hash=%016llx\n",
                name.data(), vendor.data(), effect->numPrograms,
                effect->numParams, effect->numOutputs, peak,
                static_cast<unsigned long long>(sampleHash));
    effect->dispatcher(effect, vst2::mainsChanged, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, vst2::close, 0, 0, nullptr, 0.0f);
    FreeLibrary(module);
    return peak > 0.0f && std::isfinite(peak) && timingPassed ? 0 : 6;
}
