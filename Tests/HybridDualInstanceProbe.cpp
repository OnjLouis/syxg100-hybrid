#include "../Source/Vst2Abi.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
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

class Instance {
public:
    explicit Instance(vst2::EntryPoint entry) : effect(entry(host))
    {
        if (effect == nullptr || effect->magic != vst2::effectMagic)
            throw std::runtime_error("cannot create wrapper instance");
        effect->dispatcher(effect, vst2::open, 0, 0, nullptr, 0.0f);
        effect->dispatcher(effect, vst2::setSampleRate, 0, 0, nullptr,
                           sampleRate);
        effect->dispatcher(effect, vst2::setBlockSize, 0, blockSize, nullptr,
                           0.0f);
        effect->dispatcher(effect, vst2::mainsChanged, 0, 1, nullptr, 0.0f);
    }

    ~Instance()
    {
        if (effect == nullptr)
            return;
        effect->dispatcher(effect, vst2::mainsChanged, 0, 0, nullptr, 0.0f);
        effect->dispatcher(effect, vst2::close, 0, 0, nullptr, 0.0f);
    }

    void send(vst2::Event* event)
    {
        vst2::Events events { 1 };
        events.events[0] = event;
        effect->dispatcher(effect, vst2::processEvents, 0, 0, &events, 0.0f);
        render();
    }

    void render()
    {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        std::array<float*, 2> outputs { left.data(), right.data() };
        effect->processReplacing(effect, nullptr, outputs.data(), blockSize);
        for (const auto sample : left)
            peak = std::max(peak, std::abs(sample));
        for (const auto sample : right)
            peak = std::max(peak, std::abs(sample));
    }

    float peak {};

private:
    vst2::AEffect* effect {};
    std::array<float, blockSize> left {};
    std::array<float, blockSize> right {};
};

std::vector<std::uint8_t> parseHex(std::string_view text)
{
    if ((text.size() % 2) != 0)
        throw std::runtime_error("odd event record");
    std::vector<std::uint8_t> bytes(text.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(std::stoul(
            std::string(text.substr(index * 2, 2)), nullptr, 16));
    }
    return bytes;
}

void sendRecord(Instance& first, Instance& second, const std::string& line)
{
    const auto bytes = parseHex(std::string_view(line).substr(2));
    if (line[0] == 'M') {
        vst2::MidiEvent event;
        std::copy_n(bytes.begin(), std::min<std::size_t>(bytes.size(), 4),
                    event.midiData);
        first.send(reinterpret_cast<vst2::Event*>(&event));
        second.send(reinterpret_cast<vst2::Event*>(&event));
    } else if (line[0] == 'S') {
        vst2::SysexEvent event;
        event.dumpBytes = static_cast<std::int32_t>(bytes.size());
        event.sysexDump = reinterpret_cast<char*>(
            const_cast<std::uint8_t*>(bytes.data()));
        first.send(reinterpret_cast<vst2::Event*>(&event));
        second.send(reinterpret_cast<vst2::Event*>(&event));
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: HybridDualInstanceProbe <wrapper.dll> <trace>\n");
        return 2;
    }
    const auto module = LoadLibraryA(argv[1]);
    if (module == nullptr)
        return 3;
    const auto entry = reinterpret_cast<vst2::EntryPoint>(
        GetProcAddress(module, "main"));
    if (entry == nullptr)
        return 4;

    int result = 0;
    try {
        Instance first(entry);
        Instance second(entry);
        std::ifstream input(argv[2]);
        std::string line;
        if (!input || !std::getline(input, line) || line != "PVTE 1")
            throw std::runtime_error("invalid trace");
        while (std::getline(input, line)) {
            if (line.size() >= 3 && line[1] == ' ')
                sendRecord(first, second, line);
        }
        for (int block = 0; block < 100; ++block) {
            first.render();
            second.render();
        }
        std::printf("firstPeak=%g secondPeak=%g\n", first.peak, second.peak);
        if (!(first.peak > 0.0f) || !(second.peak > 0.0f)
            || !std::isfinite(first.peak) || !std::isfinite(second.peak)) {
            result = 5;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        result = 6;
    }
    FreeLibrary(module);
    return result;
}
